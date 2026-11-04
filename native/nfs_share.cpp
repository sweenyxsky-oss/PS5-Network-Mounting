// NFS backend: libnfs behind the shared ShareBackend interface.
//
// The connect path follows PS5SX2's OrbisNfs: mount with our own deadline (libnfs's
// timeout starts at its first request, so a silent server could hold a synchronous
// nfs_mount for minutes), try the address's whole path first and then shorter exports
// with the rest treated as a folder inside, and fall back to NFS v4 when v3 gets
// nowhere. Every call into one context is serialised by NfsSession::mutex.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <nfsc/libnfs.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <vector>

#include "share.hpp"

namespace ps5nm {
namespace {

constexpr int kMountTimeoutMs = 15000;
constexpr int kIoTimeoutMs = 10000;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Fail(ShareStatus* status, int code, const std::string& message) {
    if (status) status->Fail(code, message);
}

struct NfsSession {
    std::mutex mutex;
    nfs_context* ctx = nullptr;
    bool connected = false;
    bool closing = false;
    std::string description;

    ~NfsSession() {
        if (!ctx) return;
        if (connected) (void)nfs_umount(ctx);
        nfs_destroy_context(ctx);
        ctx = nullptr;
    }
};

NodeInfo NodeFromStat(const struct nfs_stat_64& st) {
    NodeInfo node;
    node.mode = static_cast<uint32_t>(st.nfs_mode);
    node.is_dir = S_ISDIR(static_cast<mode_t>(st.nfs_mode));
    node.is_link = S_ISLNK(static_cast<mode_t>(st.nfs_mode));
    node.size = st.nfs_size;
    node.inode = st.nfs_ino;
    node.nlink = static_cast<uint32_t>(st.nfs_nlink);
    node.uid = static_cast<uint32_t>(st.nfs_uid);
    node.gid = static_cast<uint32_t>(st.nfs_gid);
    node.mtime = static_cast<int64_t>(st.nfs_mtime);
    node.mtime_nsec = static_cast<uint32_t>(st.nfs_mtime_nsec);
    return node;
}

struct MountWait {
    bool done = false;
    int status = 0;
    std::string error;
};

void MountCallback(int status, nfs_context* nfs, void* data, void* private_data) {
    auto* wait = static_cast<MountWait*>(private_data);
    if (!wait) return;
    wait->done = true;
    wait->status = status;
    if (status != 0) {
        const char* text = static_cast<const char*>(data);
        const char* detail = nfs_get_error(nfs);
        if (text && *text) {
            wait->error = text;
        } else if (detail && *detail) {
            wait->error = detail;
        } else {
            wait->error = "the server refused the mount (status " + std::to_string(status) + ")";
        }
    }
}

int MountWithDeadline(nfs_context* nfs, const std::string& host, const std::string& export_path,
                      int timeout_ms, std::string* error) {
    MountWait wait;
    if (nfs_mount_async(nfs, host.c_str(), export_path.c_str(), MountCallback, &wait) != 0) {
        if (error) *error = nfs_get_error(nfs) ? nfs_get_error(nfs) : "the mount did not start";
        return -EIO;
    }
    const double deadline = Now() + timeout_ms / 1000.0;
    while (!wait.done) {
        const double left = deadline - Now();
        if (left <= 0) {
            if (error) *error = "no answer within " + std::to_string(timeout_ms / 1000) + " s";
            return -ETIMEDOUT;
        }
        struct pollfd descriptor {
            nfs_get_fd(nfs), static_cast<short>(nfs_which_events(nfs)), 0
        };
        const int ready = poll(&descriptor, 1, std::max(1, std::min(100, static_cast<int>(left * 1000.0))));
        if (ready < 0 && errno != EINTR) {
            if (error) *error = std::string("waiting for the server failed (errno ") + std::to_string(errno) + ")";
            return -EIO;
        }
        if (nfs_service(nfs, ready > 0 ? descriptor.revents : 0) < 0 && !wait.done) {
            if (error) *error = nfs_get_error(nfs) ? nfs_get_error(nfs) : "the connection failed";
            return -EIO;
        }
    }
    if (wait.status != 0 && error) *error = wait.error;
    return wait.status;
}

nfs_context* MakeContext(const ShareSpec& spec, int version, int timeout_ms) {
    nfs_context* nfs = nfs_init_context();
    if (!nfs) return nullptr;
    (void)nfs_set_version(nfs, version);
    if (spec.port > 0) {
        nfs_set_nfsport(nfs, spec.port);
        nfs_set_mountport(nfs, spec.port);
    }
    // Do not follow exports mounted inside the share: an extra MOUNT call some servers
    // never answer.
    nfs_set_auto_traverse_mounts(nfs, 0);
    nfs_set_timeout(nfs, timeout_ms);
    return nfs;
}

bool Refusal(const std::string& error) {
    return error.find("Mount failed") != std::string::npos ||
           error.find("there is no folder") == 0 ||
           error.find("NFS4ERR") != std::string::npos;
}

}  // namespace

class NfsBackend final : public ShareBackend {
public:
    explicit NfsBackend(ShareSpec spec) : spec_(std::move(spec)) {}
    ~NfsBackend() override { Disconnect(); }

    Protocol Kind() const override { return Protocol::kNfs; }

    bool Connect(ShareStatus* status) override {
        std::vector<std::string> components;
        if (!SplitPath(spec_.remote_path, &components)) {
            Fail(status, EINVAL, "The NFS export path is not absolute.");
            return false;
        }
        if (components.empty()) components.push_back("");

        auto session = std::make_shared<NfsSession>();
        std::string last_error;
        bool silent = false;
        constexpr int version = 3;  // v3 through the portmapper; v4 is tried below

        for (size_t index = components.size(); index > 0; --index) {
            std::vector<std::string> export_parts(components.begin(), components.begin() + static_cast<long>(index));
            std::vector<std::string> inside_parts(components.begin() + static_cast<long>(index), components.end());
            const std::string export_path = JoinPath(export_parts);
            const std::string inside = inside_parts.empty() ? std::string() : JoinPath(inside_parts);

            std::string description;
            std::string why;
            nfs_context* context = nullptr;
            if (MountOne(version, export_path, inside, &context, &description, &why)) {
                session->ctx = context;
                session->connected = true;
                session->description = description;
                session_ = session;
                return true;
            }
            last_error = "NFS v" + std::to_string(version) + " " + export_path + ": " + why;
            if (Refusal(why)) continue;
            silent = silent || why.find("no answer") != std::string::npos;
            break;
        }

        if (!silent) {
            // NFS v4 only servers answer nothing on the v3 portmapper but speak on 2049.
            std::string description;
            std::string why;
            nfs_context* context = nullptr;
            const std::string export_path = spec_.remote_path;
            if (MountOne(4, export_path, "", &context, &description, &why)) {
                session->ctx = context;
                session->connected = true;
                session->description = description;
                session_ = session;
                return true;
            }
            last_error += "; then NFS v4 " + export_path + ": " + why;
        }
        Fail(status, EIO, "Could not mount the export (" + last_error + "). Check the server, the export path and the network.");
        return false;
    }

    void Disconnect() override {
        std::shared_ptr<NfsSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = std::move(session_);
        }
        if (!session) return;
        std::lock_guard<std::mutex> lock(session->mutex);
        session->closing = true;
        if (session->connected && session->ctx) {
            (void)nfs_umount(session->ctx);
        }
        session->connected = false;
        // The context itself is released by the session's last holder, so an open file
        // handle can never read from freed memory.
    }

    bool Connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_ && session_->connected;
    }

    std::string Describe() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_ ? session_->description : std::string("not connected");
    }

    bool Stat(const std::string& rel, bool follow, NodeInfo* out, ShareStatus* status) override {
        const std::shared_ptr<NfsSession> session = Current();
        if (!session || !out) {
            Fail(status, EIO, "The NFS share is not connected.");
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The NFS share was disconnected.");
            return false;
        }
        struct nfs_stat_64 st {};
        const std::string path = Remote(rel);
        const int rc = follow ? nfs_stat64(session->ctx, path.c_str(), &st)
                              : nfs_lstat64(session->ctx, path.c_str(), &st);
        if (rc != 0) {
            Fail(status, rc < 0 ? -rc : EIO, "stat " + path + ": " + ErrorText(session->ctx));
            return false;
        }
        *out = NodeFromStat(st);
        return true;
    }

    std::unique_ptr<ShareFile> Open(const std::string& rel, ShareStatus* status) override {
        const std::shared_ptr<NfsSession> session = Current();
        if (!session) {
            Fail(status, EIO, "The NFS share is not connected.");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The NFS share was disconnected.");
            return nullptr;
        }
        const std::string path = Remote(rel);
        struct nfsfh* handle = nullptr;
        const int rc = nfs_open(session->ctx, path.c_str(), O_RDONLY, &handle);
        if (rc != 0 || !handle) {
            Fail(status, rc < 0 ? -rc : EIO, "open " + path + ": " + ErrorText(session->ctx));
            return nullptr;
        }
        struct nfs_stat_64 st {};
        if (nfs_fstat64(session->ctx, handle, &st) != 0 || S_ISDIR(static_cast<mode_t>(st.nfs_mode))) {
            nfs_close(session->ctx, handle);
            Fail(status, EISDIR, path + " is not a regular file.");
            return nullptr;
        }
        return std::unique_ptr<ShareFile>(new NfsFile(session, handle, NodeFromStat(st)));
    }

    std::unique_ptr<ShareDir> OpenDir(const std::string& rel, ShareStatus* status) override {
        const std::shared_ptr<NfsSession> session = Current();
        if (!session) {
            Fail(status, EIO, "The NFS share is not connected.");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The NFS share was disconnected.");
            return nullptr;
        }
        const std::string path = Remote(rel);
        struct nfsdir* dir = nullptr;
        const int rc = nfs_opendir(session->ctx, path.c_str(), &dir);
        if (rc != 0 || !dir) {
            Fail(status, rc < 0 ? -rc : EIO, "opendir " + path + ": " + ErrorText(session->ctx));
            return nullptr;
        }
        return std::unique_ptr<ShareDir>(new NfsDir(session, dir, path));
    }

    bool Volume(const std::string& rel, VolumeInfo* out, ShareStatus* status) override {
        const std::shared_ptr<NfsSession> session = Current();
        if (!session || !out) {
            Fail(status, EIO, "The NFS share is not connected.");
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The NFS share was disconnected.");
            return false;
        }
        struct nfs_statvfs_64 vfs {};
        const std::string path = Remote(rel);
        const int rc = nfs_statvfs64(session->ctx, path.c_str(), &vfs);
        if (rc != 0) {
            Fail(status, rc < 0 ? -rc : EIO, "statvfs " + path + ": " + ErrorText(session->ctx));
            return false;
        }
        out->block_size = vfs.f_bsize;
        out->fragment_size = vfs.f_frsize;
        out->blocks = vfs.f_blocks;
        out->blocks_free = vfs.f_bfree;
        out->blocks_available = vfs.f_bavail;
        out->files = vfs.f_files;
        out->files_free = vfs.f_ffree;
        out->name_max = vfs.f_namemax;
        return true;
    }

private:
    class NfsFile final : public ShareFile {
    public:
        NfsFile(std::shared_ptr<NfsSession> session, struct nfsfh* handle, NodeInfo info)
            : session_(std::move(session)), handle_(handle), info_(info) {}

        ~NfsFile() override {
            if (!handle_) return;
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (!session_->closing && session_->ctx) (void)nfs_close(session_->ctx, handle_);
            handle_ = nullptr;
        }

        ssize_t Read(void* dst, size_t count, uint64_t offset, ShareStatus* status) override {
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->closing || !session_->ctx) {
                Fail(status, EIO, "The NFS share was disconnected.");
                return -1;
            }
            size_t done = 0;
            while (done < count) {
                const int rc = nfs_pread(session_->ctx, handle_, static_cast<char*>(dst) + done,
                                         count - done, offset + done);
                if (rc < 0) {
                    Fail(status, -rc, "read: " + ErrorText(session_->ctx));
                    return done > 0 ? static_cast<ssize_t>(done) : -1;
                }
                if (rc == 0) break;
                done += static_cast<size_t>(rc);
            }
            return static_cast<ssize_t>(done);
        }

        const NodeInfo& Info() const override { return info_; }

    private:
        std::shared_ptr<NfsSession> session_;
        struct nfsfh* handle_ = nullptr;
        NodeInfo info_;
    };

    class NfsDir final : public ShareDir {
    public:
        NfsDir(std::shared_ptr<NfsSession> session, struct nfsdir* dir, std::string path)
            : session_(std::move(session)), dir_(dir), path_(std::move(path)) {}

        ~NfsDir() override {
            if (!dir_) return;
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->ctx) nfs_closedir(session_->ctx, dir_);
            dir_ = nullptr;
        }

        bool Next(std::string* name, NodeInfo* info, ShareStatus* status) override {
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->closing || !session_->ctx) {
                Fail(status, EIO, "The NFS share was disconnected.");
                return false;
            }
            for (;;) {
                struct nfsdirent* entry = nfs_readdir(session_->ctx, dir_);
                if (!entry) return false;  // end of directory, status stays ok
                if (!entry->name || std::strcmp(entry->name, ".") == 0 ||
                    std::strcmp(entry->name, "..") == 0) {
                    continue;
                }
                if (name) *name = entry->name;
                if (info) {
                    if (entry->mode) {
                        NodeInfo node;
                        node.mode = entry->mode;
                        node.is_dir = S_ISDIR(static_cast<mode_t>(entry->mode));
                        node.is_link = S_ISLNK(static_cast<mode_t>(entry->mode));
                        node.size = entry->size;
                        node.inode = entry->inode;
                        node.nlink = entry->nlink;
                        node.uid = entry->uid;
                        node.gid = entry->gid;
                        node.mtime = entry->mtime.tv_sec;
                        node.mtime_nsec = entry->mtime_nsec;
                        *info = node;
                    } else {
                        // A READDIR that carried no attributes: ask for this one entry.
                        struct nfs_stat_64 st {};
                        const std::string full = path_ + "/" + entry->name;
                        if (nfs_stat64(session_->ctx, full.c_str(), &st) != 0) {
                            Fail(status, EIO, "stat " + full + ": " + ErrorText(session_->ctx));
                            return false;
                        }
                        *info = NodeFromStat(st);
                        if (name) *name = entry->name;
                    }
                }
                return true;
            }
        }

    private:
        std::shared_ptr<NfsSession> session_;
        struct nfsdir* dir_ = nullptr;
        std::string path_;
    };

    std::shared_ptr<NfsSession> Current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_;
    }

    std::string Remote(const std::string& rel) const {
        if (rel.empty()) return std::string("/");
        return rel.front() == '/' ? rel : "/" + rel;
    }

    static std::string ErrorText(nfs_context* ctx) {
        const char* text = nfs_get_error(ctx);
        return text && *text ? text : "the server gave no detail";
    }

    bool MountOne(int version, const std::string& export_path, const std::string& inside,
                  nfs_context** out, std::string* description, std::string* why) {
        nfs_context* nfs = MakeContext(spec_, version, kMountTimeoutMs);
        if (!nfs) {
            if (why) *why = "libnfs could not create a context";
            return false;
        }
        int rc = MountWithDeadline(nfs, spec_.host, export_path, kMountTimeoutMs, why);
        if (rc == 0 && !inside.empty()) {
            struct nfs_stat_64 st {};
            if (nfs_stat64(nfs, inside.c_str(), &st) != 0 || !S_ISDIR(static_cast<mode_t>(st.nfs_mode))) {
                if (why) *why = "there is no folder " + inside + " in " + export_path;
                rc = -ENOENT;
            }
        }
        if (rc != 0) {
            nfs_destroy_context(nfs);
            return false;
        }
        nfs_set_timeout(nfs, kIoTimeoutMs);
        if (description) {
            *description = "NFS v" + std::to_string(version) + ", export " + export_path +
                           (inside.empty() ? std::string() : ", folder " + inside);
        }
        *out = nfs;
        return true;
    }

    ShareSpec spec_;
    mutable std::mutex mutex_;
    std::shared_ptr<NfsSession> session_;
};

std::unique_ptr<ShareBackend> MakeNfsBackend(const ShareSpec& spec) {
    return std::unique_ptr<ShareBackend>(new NfsBackend(spec));
}

}  // namespace ps5nm
