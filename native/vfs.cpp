// Implementation of the in-process filesystem table. See vfs.hpp.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diagnostic_log.hpp"

namespace ps5nm {
namespace {

constexpr double kListingSeconds = 30.0;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A stable inode for a node whose server did not send one; some loaders hash it.
uint64_t StableInode(const std::string& path) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : path) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

}  // namespace

const char* ProtocolName(Protocol protocol) {
    return protocol == Protocol::kSmb ? "smb" : "nfs";
}

bool SplitPath(const std::string& path, std::vector<std::string>* components) {
    if (!components) return false;
    components->clear();
    size_t at = 0;
    while (at <= path.size()) {
        size_t end = path.find('/', at);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(at, end - at);
        at = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (components->empty()) return false;
            components->pop_back();
            continue;
        }
        components->push_back(part);
    }
    return true;
}

std::string JoinPath(const std::vector<std::string>& components) {
    std::string out;
    for (const std::string& part : components) out += "/" + part;
    return out.empty() ? std::string("/") : out;
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024) return false;
    if (path.front() == '/' || path.find('\\') != std::string::npos) return false;
    if (path.find_first_of("\r\n\0") != std::string::npos) return false;
    // Refuse a ".." component outright, even when it would still resolve inside the share.
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i] != '.' || path[i + 1] != '.') continue;
        const bool starts_component = i == 0 || path[i - 1] == '/';
        const bool ends_component = i + 2 == path.size() || path[i + 2] == '/';
        if (starts_component && ends_component) return false;
    }
    std::vector<std::string> parts;
    return SplitPath(path, &parts) && !parts.empty();
}

std::string ParentPath(const std::string& absolute_path) {
    const size_t slash = absolute_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return std::string("/");
    return absolute_path.substr(0, slash);
}

std::string BaseName(const std::string& absolute_path) {
    const size_t slash = absolute_path.find_last_of('/');
    return slash == std::string::npos ? absolute_path : absolute_path.substr(slash + 1);
}

void FillStatFromNode(const NodeInfo& node, struct stat* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    // Everything on a share is read-only here, and executable so PS5 ELFs and PRXs run.
    if (node.is_link) {
        out->st_mode = S_IFLNK | 0777;
    } else if (node.is_dir) {
        out->st_mode = S_IFDIR | 0555;
    } else {
        out->st_mode = S_IFREG | 0555;
    }
    out->st_ino = static_cast<ino_t>(node.inode ? node.inode : StableInode(std::to_string(node.size)));
    out->st_nlink = static_cast<nlink_t>(node.is_dir ? 2 : (node.nlink ? node.nlink : 1));
    out->st_uid = static_cast<uid_t>(node.uid);
    out->st_gid = static_cast<gid_t>(node.gid);
    out->st_size = static_cast<off_t>(node.size);
    out->st_blksize = 65536;
    out->st_blocks = static_cast<blkcnt_t>((node.size + 511) / 512);
    out->st_atime = static_cast<time_t>(node.mtime);
    out->st_mtime = static_cast<time_t>(node.mtime);
    out->st_ctime = static_cast<time_t>(node.mtime);
}

Vfs::Session::~Session() {
    if (backend) backend->Disconnect();
}

Vfs& Vfs::Instance() {
    static Vfs vfs;
    return vfs;
}

bool Vfs::Attached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(session_);
}

std::string Vfs::Root() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ ? session_->spec.mountpoint : std::string("/hostapp");
}

std::string Vfs::Normalise(const char* path) const {
    if (!path || path[0] != '/') return std::string();
    std::vector<std::string> parts;
    if (!SplitPath(path, &parts)) return std::string();
    return JoinPath(parts);
}

bool Vfs::IsManaged(const char* path) const {
    const std::string normal = Normalise(path);
    if (normal.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_) return false;
    const std::string& root = session_->spec.mountpoint;
    return normal == root ||
           (normal.size() > root.size() && normal.compare(0, root.size(), root) == 0 && normal[root.size()] == '/');
}

std::shared_ptr<Vfs::Session> Vfs::SessionForPath(const std::string& normal, std::string* rel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || normal.empty()) return nullptr;
    const std::string& root = session_->spec.mountpoint;
    if (normal == root) {
        if (rel) rel->clear();
        return session_;
    }
    if (normal.size() > root.size() && normal.compare(0, root.size(), root) == 0 && normal[root.size()] == '/') {
        if (rel) *rel = normal.substr(root.size());
        return session_;
    }
    return nullptr;
}

bool Vfs::Attach(std::unique_ptr<ShareBackend> backend, const ShareSpec& spec, std::string* error) {
    if (!backend) {
        if (error) *error = "No share backend was created.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_) {
            if (error) *error = "A share is already attached. Disconnect it before connecting another one.";
            return false;
        }
    }

    auto s = std::make_shared<Session>();
    s->backend = std::move(backend);
    s->spec = spec;
    s->state = s->backend->Describe();
    s->endpoint = std::string(ProtocolName(spec.protocol)) + "://" + spec.host + ":" +
                  std::to_string(spec.port) + spec.remote_path;

    ShareStatus status;
    if (!s->backend->Stat("", true, &s->root, &status)) {
        if (error) *error = "The share connected, but its root folder is unreadable: " + status.message;
        s->backend->Disconnect();
        return false;
    }
    if (!s->root.is_dir) {
        if (error) *error = "The requested share path is not a folder.";
        s->backend->Disconnect();
        return false;
    }

    const std::string endpoint = s->endpoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_) {
            if (error) *error = "A share is already attached. Disconnect it before connecting another one.";
            s->backend->Disconnect();
            return false;
        }
        session_ = std::move(s);
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        listings_.clear();
    }
    LogEvent("INFO", "vfs", "attached " + endpoint + " at " + spec.mountpoint);
    return true;
}

bool Vfs::AttachForTest(std::unique_ptr<ShareBackend> backend, const std::string& mountpoint,
                        std::string* error) {
    ShareSpec spec;
    spec.mountpoint = mountpoint;
    spec.host = "test";
    spec.remote_path = "/test";
    return Attach(std::move(backend), spec, error);
}

bool Vfs::Detach(std::string* error) {
    std::shared_ptr<Session> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_) {
            if (error) *error = "No share is attached.";
            return false;
        }
        old = std::move(session_);
        old->closing.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        listings_.clear();
    }
    old->backend->Disconnect();
    LogEvent("INFO", "vfs", "detached share at " + old->spec.mountpoint);
    return true;
}

MountStatus Vfs::Status() const {
    MountStatus out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.opens = opens_.load();
    out.reads = reads_.load();
    out.bytes = bytes_.load();
    out.requests = requests_.load();
    out.cache_hits = cache_hits_.load();
    out.open_files = open_files_.load();
    out.busy = out.open_files > 0;
    if (session_) {
        out.mounted = true;
        out.can_disconnect = true;
        out.protocol = ProtocolName(session_->spec.protocol);
        out.endpoint = session_->endpoint;
        out.mountpoint = session_->spec.mountpoint;
        out.backend_state = session_->state;
    }
    return out;
}

bool Vfs::CachedStat(const std::string& normal, NodeInfo* out) const {
    const size_t slash = normal.find_last_of('/');
    if (slash == std::string::npos || slash == 0 || !out) return false;
    const std::string folder = normal.substr(0, slash);
    const std::string name = normal.substr(slash + 1);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto it = listings_.find(folder);
    if (it == listings_.end()) return false;
    if (Now() > it->second.valid_until) {
        listings_.erase(it);
        return false;
    }
    const auto entry = it->second.names.find(name);
    cache_hits_.fetch_add(1);
    if (entry == it->second.names.end()) {
        errno = ENOENT;
        return false;
    }
    *out = entry->second;
    return true;
}

void Vfs::Remember(const std::string& folder, std::unordered_map<std::string, NodeInfo>&& names) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (listings_.size() > 4096) listings_.clear();
    Listing& listing = listings_[folder];
    listing.valid_until = Now() + kListingSeconds;
    listing.names = std::move(names);
}

int Vfs::Stat(const char* path, struct stat* out, bool follow) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    const std::string normal = Normalise(path);
    std::string rel;
    const std::shared_ptr<Session> s = SessionForPath(normal, &rel);
    if (!s) {
        errno = ENOENT;
        return -1;
    }
    if (rel.empty()) {
        FillStatFromNode(s->root, out);
        return 0;
    }

    NodeInfo node;
    if (CachedStat(normal, &node)) {
        FillStatFromNode(node, out);
        return 0;
    }

    ShareStatus status;
    {
        std::lock_guard<std::mutex> lock(s->io);
        requests_.fetch_add(1);
        if (s->closing.load()) {
            errno = EIO;
            return -1;
        }
        if (!s->backend->Stat(rel, follow, &node, &status)) {
            errno = status.code ? status.code : EIO;
            return -1;
        }
    }
    FillStatFromNode(node, out);
    return 0;
}

int Vfs::Access(const char* path, int mode) {
    struct stat st {};
    if (Stat(path, &st, true) != 0) return -1;
    if (mode & W_OK) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

int Vfs::Volume(const char* path, VolumeInfo* out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    const std::string normal = Normalise(path);
    std::string rel;
    const std::shared_ptr<Session> s = SessionForPath(normal, &rel);
    if (!s) {
        errno = ENOENT;
        return -1;
    }
    ShareStatus status;
    std::lock_guard<std::mutex> lock(s->io);
    requests_.fetch_add(1);
    if (!s->backend->Volume(rel, out, &status)) {
        errno = status.code ? status.code : EIO;
        return -1;
    }
    return 0;
}

std::unique_ptr<ShareFile> Vfs::OpenFile(const char* path, int flags, int* out_errno) {
    if (out_errno) *out_errno = 0;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        if (out_errno) *out_errno = EROFS;
        return nullptr;
    }
    const std::string normal = Normalise(path);
    std::string rel;
    const std::shared_ptr<Session> s = SessionForPath(normal, &rel);
    if (!s || (!rel.empty() && !IsManaged(path))) {
        if (out_errno) *out_errno = ENOENT;
        return nullptr;
    }
    if (rel.empty()) {
        if (out_errno) *out_errno = EISDIR;
        return nullptr;
    }
    ShareStatus status;
    std::lock_guard<std::mutex> lock(s->io);
    requests_.fetch_add(1);
    if (s->closing.load()) {
        if (out_errno) *out_errno = EIO;
        return nullptr;
    }
    auto file = s->backend->Open(rel, &status);
    if (!file) {
        if (out_errno) *out_errno = status.code ? status.code : EIO;
        return nullptr;
    }
    opens_.fetch_add(1);
    return file;
}

namespace {

// Wraps a backend listing so the entries it returns are remembered for stat().
class RecordingDir final : public ShareDir {
public:
    RecordingDir(Vfs& vfs, std::string folder, std::unique_ptr<ShareDir> inner)
        : vfs_(vfs), folder_(std::move(folder)), inner_(std::move(inner)) {}

    ~RecordingDir() override { Flush(); }

    bool Next(std::string* name, NodeInfo* info, ShareStatus* status) override {
        if (!inner_->Next(name, info, status)) {
            if (!status || status->ok()) Flush();
            return false;
        }
        if (name && info) names_[*name] = *info;
        return true;
    }

private:
    void Flush() {
        if (flushed_) return;
        flushed_ = true;
        if (!names_.empty()) vfs_.Remember(folder_, std::move(names_));
    }

    Vfs& vfs_;
    std::string folder_;
    std::unique_ptr<ShareDir> inner_;
    std::unordered_map<std::string, NodeInfo> names_;
    bool flushed_ = false;
};

}  // namespace

std::unique_ptr<ShareDir> Vfs::OpenDir(const char* path, int* out_errno) {
    if (out_errno) *out_errno = 0;
    const std::string normal = Normalise(path);
    std::string rel;
    const std::shared_ptr<Session> s = SessionForPath(normal, &rel);
    if (!s) {
        if (out_errno) *out_errno = ENOENT;
        return nullptr;
    }
    ShareStatus status;
    {
        std::lock_guard<std::mutex> lock(s->io);
        requests_.fetch_add(1);
        if (s->closing.load()) {
            if (out_errno) *out_errno = EIO;
            return nullptr;
        }
        std::unique_ptr<ShareDir> dir = s->backend->OpenDir(rel, &status);
        if (!dir) {
            if (out_errno) *out_errno = status.code ? status.code : EIO;
            return nullptr;
        }
        return std::unique_ptr<ShareDir>(new RecordingDir(*this, normal, std::move(dir)));
    }
}

void Vfs::NoteOpenFile() { open_files_.fetch_add(1); }
void Vfs::NoteClosedFile() {
    uint64_t current = open_files_.load();
    while (current > 0 && !open_files_.compare_exchange_weak(current, current - 1)) {
    }
}

}  // namespace ps5nm
