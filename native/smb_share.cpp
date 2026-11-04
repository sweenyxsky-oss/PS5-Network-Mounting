// SMB2/SMB3 backend: libsmb2 behind the shared ShareBackend interface.
//
// libsmb2 has no port setter; it parses "host:port" in the server string (see
// lib/socket.c), so a non-default port travels that way. One context is used for the
// whole share and every call into it is serialised by SmbSession::mutex.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <ctime>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "diagnostic_log.hpp"
#include "share.hpp"

namespace ps5nm {
namespace {

constexpr int kIoTimeoutSeconds = 30;

void Fail(ShareStatus* status, int code, const std::string& message) {
    if (status) status->Fail(code, message);
}

struct SmbSession {
    std::mutex mutex;
    smb2_context* ctx = nullptr;
    std::atomic<bool> connected{false};
    bool closing = false;
    std::string base;  // folder inside the share, no leading or trailing slash
    std::string description;

    ~SmbSession() {
        if (!ctx) return;
        if (connected) (void)smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        ctx = nullptr;
    }
};

NodeInfo NodeFromStat(const struct smb2_stat_64& st) {
    NodeInfo node;
    node.is_dir = st.smb2_type == SMB2_TYPE_DIRECTORY;
    node.size = node.is_dir ? 0 : st.smb2_size;
    node.inode = st.smb2_ino;
    node.nlink = st.smb2_nlink ? st.smb2_nlink : 1;
    node.mtime = static_cast<int64_t>(st.smb2_mtime);
    node.mtime_nsec = static_cast<uint32_t>(st.smb2_mtime_nsec);
    // SMB has no POSIX mode: everything is read-only and executable, like the NFS side.
    node.mode = 0555;
    return node;
}

// Server text plus the SMB status when libsmb2 has one: an empty error string with a
// status of 0xC0000034 is the difference between "wrong folder" and "server said nothing".
std::string SmbError(smb2_context* ctx) {
    const char* text = smb2_get_error(ctx);
    std::string out = text ? text : "";
    const uint32_t nt = smb2_get_nterror(ctx);
    if (nt) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08x", nt);
        if (!out.empty()) out += " ";
        out += "(SMB status ";
        out += hex;
        out += ")";
    }
    return out.empty() ? std::string("the server gave no detail") : out;
}

int SmbErrno(smb2_context* ctx) {
    const int nt = smb2_get_nterror(ctx);
    if (nt) {
        const int mapped = nterror_to_errno(static_cast<uint32_t>(nt));
        if (mapped > 0) return mapped;
    }
    return EIO;
}

NodeInfo FolderNode() {
    NodeInfo node;
    node.is_dir = true;
    node.mode = 0555;
    node.nlink = 2;
    return node;
}

// Verifies that `path` is a readable folder. libsmb2 sends a stat as an SMB2 CREATE with
// the name exactly as given, and a few servers (notably on the share root "/") answer that
// with an error while opening and listing the same folder works. Try the stat, then the
// listing, and report both failures when neither works.
bool CheckFolder(smb2_context* ctx, const std::string& path, NodeInfo* out, std::string* why) {
    struct smb2_stat_64 st {};
    if (smb2_stat(ctx, path.c_str(), &st) == 0) {
        if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
            if (out) *out = NodeFromStat(st);
            return true;
        }
        if (why) *why = "stat succeeded but the server did not mark it as a folder";
        return false;
    }
    const std::string stat_error = SmbError(ctx);

    // The root is reachable as "/" and as the empty name, depending on the server.
    const char* candidates[] = {path.c_str(), path == "/" ? "" : path.c_str()};
    for (const char* candidate : candidates) {
        struct smb2dir* dir = smb2_opendir(ctx, candidate);
        if (!dir) continue;
        smb2_closedir(ctx, dir);
        if (out) *out = FolderNode();
        return true;
    }
    if (why) {
        *why = "opening it failed too (stat: " + stat_error + "; listing: " + SmbError(ctx) + ")";
    }
    return false;
}

}  // namespace

class SmbBackend final : public ShareBackend {
public:
    explicit SmbBackend(ShareSpec spec) : spec_(std::move(spec)) {}
    ~SmbBackend() override { Disconnect(); }

    Protocol Kind() const override { return Protocol::kSmb; }

    bool Connect(ShareStatus* status) override {
        std::vector<std::string> components;
        if (!SplitPath(spec_.remote_path, &components) || components.empty()) {
            Fail(status, EINVAL, "The SMB path must be ShareName or ShareName/subfolder.");
            return false;
        }
        const std::string share = components.front();
        std::string base;
        for (size_t i = 1; i < components.size(); ++i) base += (base.empty() ? "" : "/") + components[i];

        auto session = std::make_shared<SmbSession>();
        session->base = base;
        session->ctx = smb2_init_context();
        if (!session->ctx) {
            Fail(status, ENOMEM, "libsmb2 could not create a context.");
            return false;
        }
        smb2_set_timeout(session->ctx, kIoTimeoutSeconds);
        smb2_set_security_mode(session->ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_version(session->ctx, SMB2_VERSION_ANY);

        std::string user = spec_.username.empty() ? std::string("guest") : spec_.username;
        std::string domain;
        const size_t separator = user.find_first_of("\\;");
        if (separator != std::string::npos) {
            domain = user.substr(0, separator);
            user = user.substr(separator + 1);
            if (!domain.empty()) smb2_set_domain(session->ctx, domain.c_str());
        }
        smb2_set_user(session->ctx, user.c_str());
        if (!spec_.password.empty()) smb2_set_password(session->ctx, spec_.password.c_str());

        const std::string server = spec_.port > 0 && spec_.port != 445
                                       ? spec_.host + ":" + std::to_string(spec_.port)
                                       : spec_.host;
        if (smb2_connect_share(session->ctx, server.c_str(), share.c_str(), user.c_str()) != 0) {
            const std::string why = SmbError(session->ctx);
            smb2_destroy_context(session->ctx);
            session->ctx = nullptr;
            Fail(status, EACCES, "Could not connect to \\\\" + spec_.host + "\\" + share + ": " + why +
                                     " (check the share name, user name and password).");
            return false;
        }
        session->connected = true;

        NodeInfo root;
        std::string why;
        if (!CheckFolder(session->ctx, Remote(*session, ""), &root, &why)) {
            (void)smb2_disconnect_share(session->ctx);
            session->connected = false;
            smb2_destroy_context(session->ctx);
            session->ctx = nullptr;
            Fail(status, EACCES, "The share connected, but \"" + (base.empty() ? share : base) +
                                     "\" is not a readable folder: " + why +
                                     ". Check the folder name inside the share.");
            return false;
        }
        session->description = "SMB2/3, share " + share + (base.empty() ? std::string() : ", folder " + base);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_ = session;
        }
        return true;
    }

    void Disconnect() override {
        std::shared_ptr<SmbSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = std::move(session_);
        }
        if (!session) return;
        std::lock_guard<std::mutex> lock(session->mutex);
        session->closing = true;
        if (session->connected && session->ctx) (void)smb2_disconnect_share(session->ctx);
        session->connected = false;
        // The context is released by the last holder, so open handles never read freed
        // memory; after this point their reads fail with EIO.
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
        (void)follow;  // SMB has no separate lstat
        const std::shared_ptr<SmbSession> session = Current();
        if (!session || !out) {
            Fail(status, EIO, "The SMB share is not connected.");
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The SMB share was disconnected.");
            return false;
        }
        const std::string path = Remote(*session, rel);
        if (rel.empty()) {
            std::string why;
            if (!CheckFolder(session->ctx, path, out, &why)) {
                Fail(status, EACCES, "stat " + path + ": " + why);
                return false;
            }
            return true;
        }
        struct smb2_stat_64 st {};
        if (smb2_stat(session->ctx, path.c_str(), &st) != 0) {
            Fail(status, SmbErrno(session->ctx), "stat " + path + ": " + SmbError(session->ctx));
            return false;
        }
        *out = NodeFromStat(st);
        return true;
    }

    std::unique_ptr<ShareFile> Open(const std::string& rel, ShareStatus* status) override {
        const std::shared_ptr<SmbSession> session = Current();
        if (!session) {
            Fail(status, EIO, "The SMB share is not connected.");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The SMB share was disconnected.");
            return nullptr;
        }
        const std::string path = Remote(*session, rel);
        struct smb2_stat_64 st {};
        if (smb2_stat(session->ctx, path.c_str(), &st) != 0) {
            Fail(status, SmbErrno(session->ctx), "stat " + path + ": " + SmbError(session->ctx));
            return nullptr;
        }
        if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
            Fail(status, EISDIR, path + " is a folder.");
            return nullptr;
        }
        struct smb2fh* handle = smb2_open(session->ctx, path.c_str(), O_RDONLY);
        if (!handle) {
            Fail(status, SmbErrno(session->ctx), "open " + path + ": " + SmbError(session->ctx));
            return nullptr;
        }
        NodeInfo info = NodeFromStat(st);
        return std::unique_ptr<ShareFile>(new SmbFile(session, handle, info));
    }

    std::unique_ptr<ShareDir> OpenDir(const std::string& rel, ShareStatus* status) override {
        const std::shared_ptr<SmbSession> session = Current();
        if (!session) {
            Fail(status, EIO, "The SMB share is not connected.");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The SMB share was disconnected.");
            return nullptr;
        }
        const std::string path = Remote(*session, rel);
        struct smb2dir* dir = smb2_opendir(session->ctx, path.c_str());
        if (!dir) {
            Fail(status, SmbErrno(session->ctx), "opendir " + path + ": " + SmbError(session->ctx));
            return nullptr;
        }
        return std::unique_ptr<ShareDir>(new SmbDir(session, dir, path));
    }

    bool Volume(const std::string& rel, VolumeInfo* out, ShareStatus* status) override {
        const std::shared_ptr<SmbSession> session = Current();
        if (!session || !out) {
            Fail(status, EIO, "The SMB share is not connected.");
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || !session->ctx) {
            Fail(status, EIO, "The SMB share was disconnected.");
            return false;
        }
        const std::string path = Remote(*session, rel);
        struct smb2_statvfs vfs {};
        if (smb2_statvfs(session->ctx, path.c_str(), &vfs) != 0) {
            Fail(status, SmbErrno(session->ctx), "statvfs " + path + ": " + SmbError(session->ctx));
            return false;
        }
        out->block_size = vfs.f_bsize ? vfs.f_bsize : 4096;
        out->fragment_size = vfs.f_frsize ? vfs.f_frsize : out->block_size;
        out->blocks = vfs.f_blocks;
        out->blocks_free = vfs.f_bfree;
        out->blocks_available = vfs.f_bavail;
        out->files = vfs.f_files;
        out->files_free = vfs.f_ffree;
        out->name_max = vfs.f_namemax ? vfs.f_namemax : 255;
        return true;
    }

private:
    class SmbFile final : public ShareFile {
    public:
        SmbFile(std::shared_ptr<SmbSession> session, struct smb2fh* handle, NodeInfo info)
            : session_(std::move(session)), handle_(handle), info_(info) {}

        ~SmbFile() override {
            if (!handle_) return;
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (!session_->closing && session_->ctx) (void)smb2_close(session_->ctx, handle_);
            handle_ = nullptr;
        }

        ssize_t Read(void* dst, size_t count, uint64_t offset, ShareStatus* status) override {
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->closing || !session_->ctx) {
                Fail(status, EIO, "The SMB share was disconnected.");
                return -1;
            }
            size_t done = 0;
            while (done < count) {
                const int rc = smb2_pread(session_->ctx, handle_,
                                          static_cast<uint8_t*>(dst) + done,
                                          static_cast<uint32_t>(count - done), offset + done);
                if (rc < 0) {
                    Fail(status, SmbErrno(session_->ctx), "read: " + SmbError(session_->ctx));
                    return done > 0 ? static_cast<ssize_t>(done) : -1;
                }
                if (rc == 0) break;
                done += static_cast<size_t>(rc);
            }
            return static_cast<ssize_t>(done);
        }

        const NodeInfo& Info() const override { return info_; }

    private:
        std::shared_ptr<SmbSession> session_;
        struct smb2fh* handle_ = nullptr;
        NodeInfo info_;
    };

    class SmbDir final : public ShareDir {
    public:
        SmbDir(std::shared_ptr<SmbSession> session, struct smb2dir* dir, std::string path)
            : session_(std::move(session)), dir_(dir), path_(std::move(path)) {}

        ~SmbDir() override {
            if (!dir_) return;
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->ctx) smb2_closedir(session_->ctx, dir_);
            dir_ = nullptr;
        }

        bool Next(std::string* name, NodeInfo* info, ShareStatus* status) override {
            std::lock_guard<std::mutex> lock(session_->mutex);
            if (session_->closing || !session_->ctx) {
                Fail(status, EIO, "The SMB share was disconnected.");
                return false;
            }
            for (;;) {
                struct smb2dirent* entry = smb2_readdir(session_->ctx, dir_);
                if (!entry) return false;  // end of directory
                if (!entry->name || std::strcmp(entry->name, ".") == 0 ||
                    std::strcmp(entry->name, "..") == 0) {
                    continue;
                }
                if (name) *name = entry->name;
                if (info) *info = NodeFromStat(entry->st);
                return true;
            }
        }

    private:
        std::shared_ptr<SmbSession> session_;
        struct smb2dir* dir_ = nullptr;
        std::string path_;
    };

    std::shared_ptr<SmbSession> Current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_;
    }

    // "/" + base + rel, always absolute: libsmb2 accepts "/" for the share root.
    static std::string Remote(const SmbSession& session, const std::string& rel) {
        std::string path = "/";
        path += session.base;
        path += rel;
        return path;
    }

    ShareSpec spec_;
    mutable std::mutex mutex_;
    std::shared_ptr<SmbSession> session_;
};

std::unique_ptr<ShareBackend> MakeSmbBackend(const ShareSpec& spec) {
    return std::unique_ptr<ShareBackend>(new SmbBackend(spec));
}

}  // namespace ps5nm
