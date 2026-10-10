#include "mount_backend.hpp"
#ifdef PS5NM_WITH_FUSE
#include "nfs_fuse.hpp"
#endif

namespace ps5nm {

MountStatus MountBackend::Status() const {
    MountStatus current = status_;
#ifdef PS5NM_WITH_FUSE
    if (current.mounted && !NfsFuseIsMounted()) {
        current.mounted = false;
        current.last_result = "NFS mount stopped";
        current.last_error = "The FUSE request loop is no longer active. Disconnect to clean up, then reconnect.";
    }
#endif
    return current;
}

bool MountBackend::Mount(const std::string& protocol, const std::string& host, int port,
                         const std::string& remote_path, const std::string& mountpoint,
                         std::string* error) {
    if (protocol != "nfs" && protocol != "smb" && protocol != "ftp" && protocol != "sftp") {
        if (error) *error = "Unknown protocol. Choose NFS, SMB, FTP, or SFTP.";
        status_.last_error = error ? *error : "Unknown protocol.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages /hostapp.";
        status_.last_error = error ? *error : "Unsafe mountpoint.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (host.empty() || host.size() > 253 || host.find_first_of(" /\\\r\n\t") != std::string::npos) {
        if (error) *error = "Enter a hostname or IPv4 address only.";
        status_.last_error = error ? *error : "Invalid host.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (port < 1 || port > 65535 || remote_path.empty() || remote_path.front() != '/' ||
        remote_path.find("..") != std::string::npos || remote_path.find_first_of("?#") != std::string::npos) {
        if (error) *error = "Invalid port or export path. Use an absolute NFS export path without '..', '?' or '#'.";
        status_.last_error = error ? *error : "Invalid path or port.";
        status_.last_result = "Mount rejected";
        return false;
    }

    if (protocol == "nfs") {
#ifdef PS5NM_WITH_FUSE
        if (!StartNfsFuse(host, port, remote_path, mountpoint, error)) {
            status_.last_error = error ? *error : "NFS mount failed.";
            status_.last_result = "NFS mount failed";
            status_.mounted = false;
            return false;
        }
        status_.mounted = true;
        status_.protocol = "nfs";
        status_.endpoint = "nfs://" + host + ":" + std::to_string(port) + remote_path;
        status_.mountpoint = mountpoint;
        status_.last_result = "NFS share mounted read-only.";
        status_.last_error.clear();
        return true;
#else
        if (error) *error = "This build has no PS5 FUSE/NFS adapter. Rebuild with PS5NM_WITH_FUSE=1 and the compatible ORBISDEV libraries.";
        status_.last_error = error ? *error : "NFS adapter not linked.";
        status_.last_result = "Mount not performed";
        return false;
#endif
    }

    if (error) *error = "SMB, FTP, and SFTP filesystem adapters are not implemented. Remote browsing is not a local mount.";
    status_.last_error = error ? *error : "Filesystem adapter unavailable.";
    status_.last_result = "Mount not performed";
    return false;
}

bool MountBackend::Unmount(const std::string& mountpoint, std::string* error) {
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages the /hostapp mountpoint.";
        return false;
    }
#ifdef PS5NM_WITH_FUSE
    if (status_.protocol == "nfs" && (status_.mounted || NfsFuseIsMounted())) {
        if (!StopNfsFuse(mountpoint, error)) {
            status_.last_error = error ? *error : "NFS unmount failed.";
            status_.last_result = "Unmount failed";
            return false;
        }
        status_.mounted = false;
        status_.protocol.clear();
        status_.endpoint.clear();
        status_.last_result = "NFS share disconnected.";
        status_.last_error.clear();
        return true;
    }
#endif
    if (!status_.mounted) {
        if (error) *error = "No mount owned by this payload is active.";
        status_.last_result = "Nothing to disconnect";
        return false;
    }
    if (error) *error = "The adapter that created this mount is not available in this build.";
    return false;
}

} // namespace ps5nm
