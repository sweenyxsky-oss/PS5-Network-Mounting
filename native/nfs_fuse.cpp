#include "nfs_fuse.hpp"

#define FUSE_USE_VERSION 26
#include <fuse/fuse.h>
#include <nfsc/libnfs.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

extern "C" int fuse_fw_supported(void);

namespace ps5nm {
namespace {
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
struct nfs_context* g_nfs = nullptr;
struct nfs_url* g_url = nullptr;
struct fuse_chan* g_channel = nullptr;
struct fuse* g_fuse = nullptr;
pthread_t g_loop_thread{};
bool g_thread_started = false;
bool g_loop_alive = false;
bool g_mounted = false;
bool g_nfs_connected = false;
std::string g_mountpoint;
std::string g_last_error;

bool SafePath(const char* path) {
    if (!path || path[0] != '/') return false;
    const std::string s(path);
    if (s.size() > 4096 || s.find('\\') != std::string::npos ||
        s.find_first_of("\r\n") != std::string::npos) return false;
    size_t start = 1;
    while (start <= s.size()) {
        const size_t end = s.find('/', start);
        const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
std::string NfsPath(const char* path) {
    std::string relative = path ? path : "/";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    return relative;
}
bool MountpointIsEmpty(const std::string& mountpoint, std::string* error) {
    DIR* dir = opendir(mountpoint.c_str());
    if (!dir) {
        if (error) *error = "Cannot open /hostapp. Verify that the directory exists and is accessible.";
        return false;
    }
    bool empty = true;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") != 0 && std::strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    if (!empty && error)
        *error = "/hostapp is not empty. Move its contents elsewhere before mounting; this payload will not hide existing files.";
    return empty;
}
int FsGetattr(const char* path, struct stat* st) {
    if (!SafePath(path) || !st) return -EINVAL;
    struct nfs_stat_64 ns{};
    const std::string remote = NfsPath(path);
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_stat64(g_nfs, remote.c_str(), &ns);
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) return rc;
    std::memset(st, 0, sizeof(*st));
    st->st_ino = static_cast<ino_t>(ns.nfs_ino);
    st->st_mode = static_cast<mode_t>(ns.nfs_mode);
    st->st_nlink = static_cast<nlink_t>(ns.nfs_nlink);
    st->st_uid = static_cast<uid_t>(ns.nfs_uid);
    st->st_gid = static_cast<gid_t>(ns.nfs_gid);
    st->st_size = static_cast<off_t>(ns.nfs_size);
    st->st_blksize = static_cast<blksize_t>(ns.nfs_blksize ? ns.nfs_blksize : 4096);
    st->st_blocks = static_cast<blkcnt_t>(ns.nfs_blocks);
    st->st_atime = static_cast<time_t>(ns.nfs_atime);
    st->st_mtime = static_cast<time_t>(ns.nfs_mtime);
    st->st_ctime = static_cast<time_t>(ns.nfs_ctime);
    return 0;
}
int FsReadlink(const char* path, char* buf, size_t size) {
    if (!SafePath(path) || !buf || size == 0 || size > 1024 * 1024) return -EINVAL;
    const std::string remote = NfsPath(path);
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_readlink(g_nfs, remote.c_str(), buf, static_cast<int>(size - 1));
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) return rc;
    buf[size - 1] = '\0';
    return 0;
}
int FsReaddir(const char* path, void* buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info*) {
    if (!SafePath(path) || offset < 0) return -EINVAL;
    const std::string remote = NfsPath(path);
    struct nfsdir* dir = nullptr;
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_opendir(g_nfs, remote.c_str(), &dir);
    if (rc < 0) {
        pthread_mutex_unlock(&g_lock);
        return rc;
    }
    struct nfsdirent* ent = nullptr;
    off_t index = 0;
    while ((ent = nfs_readdir(g_nfs, dir)) != nullptr) {
        if (index++ < offset) continue;
        if (filler(buffer, ent->name, nullptr, index) != 0) break;
    }
    nfs_closedir(g_nfs, dir);
    pthread_mutex_unlock(&g_lock);
    return 0;
}
int FsOpen(const char* path, struct fuse_file_info* fi) {
    if (!SafePath(path) || !fi) return -EINVAL;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    struct nfsfh* fh = nullptr;
    const std::string remote = NfsPath(path);
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_open(g_nfs, remote.c_str(), O_RDONLY, &fh);
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) return rc;
    fi->fh = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fh));
    return 0;
}
int FsRead(const char*, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    if (!buf || !fi || offset < 0 || size > 8 * 1024 * 1024) return -EINVAL;
    auto* fh = reinterpret_cast<struct nfsfh*>(static_cast<uintptr_t>(fi->fh));
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_pread(g_nfs, fh, buf, size, static_cast<uint64_t>(offset));
    pthread_mutex_unlock(&g_lock);
    return rc;
}
int FsRelease(const char*, struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* fh = reinterpret_cast<struct nfsfh*>(static_cast<uintptr_t>(fi->fh));
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_close(g_nfs, fh);
    pthread_mutex_unlock(&g_lock);
    fi->fh = 0;
    return rc;
}
int FsStatfs(const char* path, struct statvfs* st) {
    if (!SafePath(path) || !st) return -EINVAL;
    const std::string remote = NfsPath(path);
    struct nfs_statvfs_64 ns{};
    pthread_mutex_lock(&g_lock);
    const int rc = nfs_statvfs64(g_nfs, remote.c_str(), &ns);
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) return rc;
    std::memset(st, 0, sizeof(*st));
    st->f_bsize = static_cast<unsigned long>(ns.f_bsize);
    st->f_frsize = static_cast<unsigned long>(ns.f_frsize);
    st->f_blocks = static_cast<fsblkcnt_t>(ns.f_blocks);
    st->f_bfree = static_cast<fsblkcnt_t>(ns.f_bfree);
    st->f_bavail = static_cast<fsblkcnt_t>(ns.f_bavail);
    st->f_files = static_cast<fsfilcnt_t>(ns.f_files);
    st->f_ffree = static_cast<fsfilcnt_t>(ns.f_ffree);
    st->f_favail = static_cast<fsfilcnt_t>(ns.f_favail);
    st->f_namemax = static_cast<unsigned long>(ns.f_namemax);
    return 0;
}
struct fuse_operations g_ops = {};
void* FuseLoop(void*) {
    const int rc = fuse_loop(g_fuse);
    pthread_mutex_lock(&g_lock);
    g_loop_alive = false;
    if (rc != 0) g_last_error = "FUSE loop stopped with error " + std::to_string(rc);
    pthread_mutex_unlock(&g_lock);
    return nullptr;
}
void CleanupNfs() {
    if (g_nfs && g_nfs_connected) (void)nfs_umount(g_nfs);
    g_nfs_connected = false;
    if (g_url) { nfs_destroy_url(g_url); g_url = nullptr; }
    if (g_nfs) { nfs_destroy_context(g_nfs); g_nfs = nullptr; }
}
}

bool StartNfsFuse(const std::string& host, int port, const std::string& export_path,
                  const std::string& mountpoint, std::string* error) {
    if (host.empty() || host.find_first_of(":/\\ \t\r\n") != std::string::npos) {
        if (error) *error = "Use an IPv4 address or DNS hostname (IPv6 literals are not supported yet).";
        return false;
    }
    if (port < 1 || port > 65535 || export_path.empty() || export_path[0] != '/' ||
        export_path.find("..") != std::string::npos || export_path.find_first_of("?#") != std::string::npos || mountpoint != "/hostapp") {
        if (error) *error = "Invalid NFS port, export path, or mountpoint. Only /hostapp is allowed.";
        return false;
    }
    pthread_mutex_lock(&g_lock);
    if (g_mounted || g_fuse || g_nfs) {
        pthread_mutex_unlock(&g_lock);
        if (error) *error = "A network filesystem is already active or starting.";
        return false;
    }
    pthread_mutex_unlock(&g_lock);
    if (!fuse_fw_supported()) {
        if (error) *error = "The installed FUSE layer reports that this firmware is unsupported.";
        return false;
    }
    struct stat existing{};
    if (stat(mountpoint.c_str(), &existing) != 0 || !S_ISDIR(existing.st_mode)) {
        if (error) *error = "The /hostapp directory does not exist or is not a directory; no files were changed.";
        return false;
    }
    if (!MountpointIsEmpty(mountpoint, error)) return false;

    std::string url = "nfs://" + host + export_path + "?nfsport=" + std::to_string(port);
    g_nfs = nfs_init_context();
    if (!g_nfs) {
        if (error) *error = "libnfs could not initialize an NFS context.";
        return false;
    }
    g_url = nfs_parse_url_dir(g_nfs, url.c_str());
    if (!g_url) {
        if (error) *error = std::string("Invalid NFS export URL: ") + nfs_get_error(g_nfs);
        CleanupNfs();
        return false;
    }
    const int nrc = nfs_mount(g_nfs, g_url->server, g_url->path);
    if (nrc != 0) {
        if (error) *error = std::string("NFS export mount failed: ") + nfs_get_error(g_nfs);
        CleanupNfs();
        return false;
    }
    g_nfs_connected = true;
    struct nfs_stat_64 root_stat{};
    if (nfs_stat64(g_nfs, "", &root_stat) != 0) {
        if (error) *error = std::string("NFS export mounted but root metadata is unreadable: ") + nfs_get_error(g_nfs);
        CleanupNfs();
        return false;
    }

    g_ops.getattr = FsGetattr;
    g_ops.readlink = FsReadlink;
    g_ops.readdir = FsReaddir;
    g_ops.open = FsOpen;
    g_ops.read = FsRead;
    g_ops.release = FsRelease;
    g_ops.statfs = FsStatfs;

    char arg0[] = "ps5-network-mounting";
    char arg1[] = "-o";
    char arg2[] = "ro";
    char arg3[] = "-o";
    char arg4[] = "allow_other";
    char arg5[] = "-f";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(6, argv);
    g_channel = fuse_mount(mountpoint.c_str(), &args);
    if (!g_channel) {
        if (error) *error = "FUSE could not attach to /hostapp. Check firmware support and whether another mount owns that path.";
        fuse_opt_free_args(&args);
        CleanupNfs();
        return false;
    }
    g_fuse = fuse_new(g_channel, &args, &g_ops, sizeof(g_ops), nullptr);
    fuse_opt_free_args(&args);
    if (!g_fuse) {
        if (error) *error = "FUSE mounted the channel but could not initialize filesystem operations.";
        fuse_unmount(mountpoint.c_str(), g_channel);
        g_channel = nullptr;
        CleanupNfs();
        return false;
    }
    g_mountpoint = mountpoint;
    g_loop_alive = true;
    if (pthread_create(&g_loop_thread, nullptr, FuseLoop, nullptr) != 0) {
        g_loop_alive = false;
        if (error) *error = "Could not start the FUSE request loop.";
        fuse_unmount(mountpoint.c_str(), g_channel);
        fuse_destroy(g_fuse);
        g_fuse = nullptr; g_channel = nullptr;
        CleanupNfs();
        return false;
    }
    g_thread_started = true;
    pthread_mutex_lock(&g_lock);
    g_mounted = true;
    g_last_error.clear();
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool StopNfsFuse(const std::string& mountpoint, std::string* error) {
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages /hostapp.";
        return false;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_fuse || !g_channel) {
        pthread_mutex_unlock(&g_lock);
        if (error) *error = "No NFS mount owned by this payload is active.";
        return false;
    }
    g_mounted = false;
    fuse_exit(g_fuse);
    pthread_mutex_unlock(&g_lock);
    fuse_unmount(mountpoint.c_str(), g_channel);
    if (g_thread_started) pthread_join(g_loop_thread, nullptr);
    fuse_destroy(g_fuse);
    g_fuse = nullptr; g_channel = nullptr; g_thread_started = false;
    CleanupNfs();
    g_mountpoint.clear();
    return true;
}

bool NfsFuseIsMounted() {
    pthread_mutex_lock(&g_lock);
    const bool mounted = g_mounted && g_loop_alive && g_fuse && g_channel;
    pthread_mutex_unlock(&g_lock);
    return mounted;
}
}
