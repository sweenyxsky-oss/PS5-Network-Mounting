#include "smb_fuse.hpp"

#define FUSE_USE_VERSION 26
#include <fuse/fuse.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

extern "C" int fuse_fw_supported(void);

namespace ps5nm {
namespace {
pthread_mutex_t g_smb_lock = PTHREAD_MUTEX_INITIALIZER;
struct smb2_context* g_smb = nullptr;
struct fuse_chan* g_smb_channel = nullptr;
struct fuse* g_smb_fuse = nullptr;
pthread_t g_smb_thread{};
bool g_smb_thread_started = false;
bool g_smb_loop_alive = false;
bool g_smb_mounted = false;
std::string g_smb_mountpoint;
std::string g_smb_root;
std::string g_smb_error;

bool SafePath(const char* path) {
    if (!path || path[0] != '/') return false;
    const std::string s(path);
    if (s.size() > 4096 || s.find('\\\\') != std::string::npos ||
        s.find_first_of("\\r\\n") != std::string::npos) return false;
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

std::string RemotePath(const char* path) {
    std::string relative = path ? path : "/";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (g_smb_root.empty()) return relative;
    if (relative.empty()) return g_smb_root;
    return g_smb_root + "/" + relative;
}

void FillStat(const struct smb2_stat_64& src, struct stat* dst) {
    std::memset(dst, 0, sizeof(*dst));
    dst->st_ino = static_cast<ino_t>(src.smb2_ino);
    dst->st_nlink = static_cast<nlink_t>(src.smb2_nlink ? src.smb2_nlink : 1);
    dst->st_uid = 0;
    dst->st_gid = 0;
    dst->st_size = static_cast<off_t>(src.smb2_size);
    dst->st_blksize = 4096;
    dst->st_blocks = static_cast<blkcnt_t>((src.smb2_size + 511) / 512);
    dst->st_atime = static_cast<time_t>(src.smb2_atime);
    dst->st_mtime = static_cast<time_t>(src.smb2_mtime);
    dst->st_ctime = static_cast<time_t>(src.smb2_ctime);
    if (src.smb2_type == SMB2_TYPE_DIRECTORY)
        dst->st_mode = S_IFDIR | 0555;
    else if (src.smb2_type == SMB2_TYPE_LINK)
        dst->st_mode = S_IFLNK | 0444;
    else
        dst->st_mode = S_IFREG | 0444;
}

int SmbGetattr(const char* path, struct stat* st) {
    if (!SafePath(path) || !st) return -EINVAL;
    const std::string remote = RemotePath(path);
    struct smb2_stat_64 remote_stat{};
    pthread_mutex_lock(&g_smb_lock);
    const int rc = smb2_stat(g_smb, remote.c_str(), &remote_stat);
    if (rc == 0) FillStat(remote_stat, st);
    pthread_mutex_unlock(&g_smb_lock);
    return rc;
}

int SmbReaddir(const char* path, void* buffer, fuse_fill_dir_t filler, off_t offset,
               struct fuse_file_info*) {
    if (!SafePath(path) || offset < 0) return -EINVAL;
    const std::string remote = RemotePath(path);
    pthread_mutex_lock(&g_smb_lock);
    struct smb2dir* dir = smb2_opendir(g_smb, remote.c_str());
    if (!dir) {
        pthread_mutex_unlock(&g_smb_lock);
        return -EIO;
    }
    if (offset > 0) smb2_seekdir(g_smb, dir, static_cast<long>(offset));
    int result = 0;
    struct smb2dirent* ent = nullptr;
    while ((ent = smb2_readdir(g_smb, dir)) != nullptr) {
        const long next = smb2_telldir(g_smb, dir);
        struct stat st{};
        FillStat(ent->st, &st);
        if (filler(buffer, ent->name, &st, static_cast<off_t>(next)) != 0) break;
    }
    smb2_closedir(g_smb, dir);
    pthread_mutex_unlock(&g_smb_lock);
    return result;
}

int SmbOpen(const char* path, struct fuse_file_info* fi) {
    if (!SafePath(path) || !fi) return -EINVAL;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    const std::string remote = RemotePath(path);
    pthread_mutex_lock(&g_smb_lock);
    struct smb2fh* fh = smb2_open(g_smb, remote.c_str(), O_RDONLY);
    const char* err = fh ? nullptr : smb2_get_error(g_smb);
    if (fh) fi->fh = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fh));
    pthread_mutex_unlock(&g_smb_lock);
    if (!fh) {
        (void)err;
        return -EACCES;
    }
    return 0;
}

int SmbRead(const char*, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    if (!buf || !fi || offset < 0 || size > 8 * 1024 * 1024) return -EINVAL;
    auto* fh = reinterpret_cast<struct smb2fh*>(static_cast<uintptr_t>(fi->fh));
    if (!fh) return -EBADF;
    pthread_mutex_lock(&g_smb_lock);
    int rc = smb2_pread(g_smb, fh, reinterpret_cast<uint8_t*>(buf),
                        static_cast<uint32_t>(size), static_cast<uint64_t>(offset));
    pthread_mutex_unlock(&g_smb_lock);
    return rc == -EAGAIN ? 0 : rc;
}

int SmbRelease(const char*, struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* fh = reinterpret_cast<struct smb2fh*>(static_cast<uintptr_t>(fi->fh));
    if (!fh) return 0;
    pthread_mutex_lock(&g_smb_lock);
    const int rc = smb2_close(g_smb, fh);
    pthread_mutex_unlock(&g_smb_lock);
    fi->fh = 0;
    return rc;
}

int SmbStatfs(const char* path, struct statvfs* st) {
    if (!SafePath(path) || !st) return -EINVAL;
    const std::string remote = RemotePath(path);
    struct smb2_statvfs src{};
    pthread_mutex_lock(&g_smb_lock);
    const int rc = smb2_statvfs(g_smb, remote.c_str(), &src);
    pthread_mutex_unlock(&g_smb_lock);
    if (rc != 0) return rc;
    std::memset(st, 0, sizeof(*st));
    st->f_bsize = src.f_bsize ? src.f_bsize : 4096;
    st->f_frsize = src.f_frsize ? src.f_frsize : st->f_bsize;
    st->f_blocks = static_cast<fsblkcnt_t>(src.f_blocks);
    st->f_bfree = static_cast<fsblkcnt_t>(src.f_bfree);
    st->f_bavail = static_cast<fsblkcnt_t>(src.f_bavail);
    st->f_files = static_cast<fsfilcnt_t>(src.f_files);
    st->f_ffree = static_cast<fsfilcnt_t>(src.f_ffree);
    st->f_favail = static_cast<fsfilcnt_t>(src.f_favail);
    st->f_namemax = src.f_namemax ? src.f_namemax : 255;
    return 0;
}

struct fuse_operations g_smb_ops = {};
void* SmbLoop(void*) {
    const int rc = fuse_loop(g_smb_fuse);
    pthread_mutex_lock(&g_smb_lock);
    g_smb_loop_alive = false;
    if (rc != 0) g_smb_error = "SMB FUSE request loop stopped with error " + std::to_string(rc);
    pthread_mutex_unlock(&g_smb_lock);
    return nullptr;
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

void CleanupSmb() {
    if (g_smb) {
        (void)smb2_disconnect_share(g_smb);
        smb2_destroy_context(g_smb);
        g_smb = nullptr;
    }
    g_smb_root.clear();
}
}

bool StartSmbFuse(const std::string& host, int port, const std::string& share_and_path,
                  const std::string& mountpoint, const std::string& username,
                  const std::string& password, std::string* error) {
    if (host.empty() || host.size() > 253 ||
        host.find_first_of(":/\\ \\t\\r\\n@;") != std::string::npos) {
        if (error) *error = "Enter an IPv4 address or DNS hostname. IPv6 literals are not supported yet.";
        return false;
    }
    if (port < 1 || port > 65535 || mountpoint != "/hostapp" ||
        share_and_path.empty() || share_and_path.front() == '/' ||
        share_and_path.find("..") != std::string::npos ||
        share_and_path.find_first_of("?#\\\\\\r\\n") != std::string::npos) {
        if (error) *error = "Invalid SMB port, share path, or mountpoint. Enter ShareName or ShareName/subfolder.";
        return false;
    }
    pthread_mutex_lock(&g_smb_lock);
    if (g_smb_mounted || g_smb_fuse || g_smb) {
        pthread_mutex_unlock(&g_smb_lock);
        if (error) *error = "An SMB filesystem is already active or starting.";
        return false;
    }
    pthread_mutex_unlock(&g_smb_lock);
    if (!fuse_fw_supported()) {
        if (error) *error = "The installed FUSE layer reports that this firmware is unsupported.";
        return false;
    }
    if (!MountpointIsEmpty(mountpoint, error)) return false;

    std::string url_text = "smb://" + host + ":" + std::to_string(port) + "/" + share_and_path;
    g_smb = smb2_init_context();
    if (!g_smb) {
        if (error) *error = "libsmb2 could not initialize an SMB context.";
        return false;
    }
    smb2_set_timeout(g_smb, 12);
    smb2_set_security_mode(g_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (!username.empty()) smb2_set_user(g_smb, username.c_str());
    if (!password.empty()) smb2_set_password(g_smb, password.c_str());
    struct smb2_url* url = smb2_parse_url(g_smb, url_text.c_str());
    if (!url || !url->server || !url->share) {
        if (error) *error = std::string("Invalid SMB share URL: ") + smb2_get_error(g_smb);
        if (url) smb2_destroy_url(url);
        CleanupSmb();
        return false;
    }
    const std::string root = url->path ? url->path : "";
    if (smb2_connect_share(g_smb, url->server, url->share,
                           username.empty() ? nullptr : username.c_str()) != 0) {
        if (error) *error = std::string("SMB connection or authentication failed: ") + smb2_get_error(g_smb);
        smb2_destroy_url(url);
        CleanupSmb();
        return false;
    }
    smb2_destroy_url(url);
    g_smb_root = root;
    g_smb_ops.getattr = SmbGetattr;
    g_smb_ops.readdir = SmbReaddir;
    g_smb_ops.open = SmbOpen;
    g_smb_ops.read = SmbRead;
    g_smb_ops.release = SmbRelease;
    g_smb_ops.statfs = SmbStatfs;

    char arg0[] = "ps5-network-mounting";
    char arg1[] = "-o";
    char arg2[] = "ro";
    char arg3[] = "-o";
    char arg4[] = "allow_other";
    char arg5[] = "-f";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(6, argv);
    g_smb_channel = fuse_mount(mountpoint.c_str(), &args);
    if (!g_smb_channel) {
        if (error) *error = "FUSE could not attach to /hostapp. Check firmware support and whether another mount owns that path.";
        fuse_opt_free_args(&args);
        CleanupSmb();
        return false;
    }
    g_smb_fuse = fuse_new(g_smb_channel, &args, &g_smb_ops, sizeof(g_smb_ops), nullptr);
    fuse_opt_free_args(&args);
    if (!g_smb_fuse) {
        if (error) *error = "FUSE attached the channel but could not initialize SMB filesystem operations.";
        fuse_unmount(mountpoint.c_str(), g_smb_channel);
        g_smb_channel = nullptr;
        CleanupSmb();
        return false;
    }
    g_smb_mountpoint = mountpoint;
    g_smb_loop_alive = true;
    if (pthread_create(&g_smb_thread, nullptr, SmbLoop, nullptr) != 0) {
        g_smb_loop_alive = false;
        if (error) *error = "Could not start the SMB FUSE request loop.";
        fuse_unmount(mountpoint.c_str(), g_smb_channel);
        fuse_destroy(g_smb_fuse);
        g_smb_fuse = nullptr;
        g_smb_channel = nullptr;
        CleanupSmb();
        return false;
    }
    g_smb_thread_started = true;
    pthread_mutex_lock(&g_smb_lock);
    g_smb_mounted = true;
    g_smb_error.clear();
    pthread_mutex_unlock(&g_smb_lock);
    return true;
}

bool StopSmbFuse(const std::string& mountpoint, std::string* error) {
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages /hostapp.";
        return false;
    }
    pthread_mutex_lock(&g_smb_lock);
    if (!g_smb_fuse || !g_smb_channel) {
        pthread_mutex_unlock(&g_smb_lock);
        if (error) *error = "No SMB mount owned by this payload is active.";
        return false;
    }
    g_smb_mounted = false;
    fuse_exit(g_smb_fuse);
    pthread_mutex_unlock(&g_smb_lock);
    fuse_unmount(mountpoint.c_str(), g_smb_channel);
    if (g_smb_thread_started) pthread_join(g_smb_thread, nullptr);
    fuse_destroy(g_smb_fuse);
    g_smb_fuse = nullptr;
    g_smb_channel = nullptr;
    g_smb_thread_started = false;
    CleanupSmb();
    g_smb_mountpoint.clear();
    return true;
}

bool SmbFuseIsMounted() {
    pthread_mutex_lock(&g_smb_lock);
    const bool mounted = g_smb_mounted && g_smb_loop_alive && g_smb_fuse && g_smb_channel;
    pthread_mutex_unlock(&g_smb_lock);
    return mounted;
}
}
