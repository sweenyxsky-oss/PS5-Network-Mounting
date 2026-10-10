#include "mount_backend.hpp"
#ifdef PS5NM_WITH_FUSE
#include "nfs_fuse.hpp"
#include "smb_fuse.hpp"
#endif

namespace ps5nm {

MountStatus MountBackend::Status() const {
    MountStatus current = status_;
    current.can_disconnect = status_.mounted || !status_.protocol.empty();
#ifdef PS5NM_WITH_FUSE
    if (current.mounted && current.protocol == "nfs" && !NfsFuseIsMounted()) {
        current.mounted = false;
        current.last_result = "NFS mount stopped";
        current.last_error = "The FUSE request loop is no longer active. Disconnect to clean up, then reconnect.";
    }
    if (current.mounted && current.protocol == "smb" && !SmbFuseIsMounted()) {
        current.mounted = false;
        current.last_result = "SMB mount stopped";
        current.last_error = "The FUSE request loop is no longer active. Disconnect to clean up, then reconnect.";
    }
#endif
    return current;
}

bool MountBackend::Mount(const std::string& protocol, const std::string& host, int port,
                         const std::string& remote_path, const std::string& mountpoint,
                         const std::string& username, const std::string& password,
                         std::string* error) {
    if (protocol != "nfs" && protocol != "smb") {
        if (error) *error = "Unsupported protocol. Choose NFS or SMB.";
        status_.last_error = error ? *error : "Unsupported protocol.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (status_.mounted) {
        if (error) *error = "A network share is already mounted. Disconnect it before connecting another share.";
        status_.last_error = error ? *error : "Another share is mounted.";
        status_.last_result = "Mount rejected";
        return false;
    }
#ifdef PS5NM_WITH_FUSE
    if ((status_.protocol == "nfs" && NfsFuseIsMounted()) ||
        (status_.protocol == "smb" && SmbFuseIsMounted())) {
        if (error) *error = "A network share is already mounted. Disconnect it before connecting another share.";
        status_.last_error = error ? *error : "Another share is mounted.";
        status_.last_result = "Mount rejected";
        return false;
    }
#endif
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages /hostapp.";
        status_.last_error = error ? *error : "Unsafe mountpoint.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (host.empty() || host.size() > 253 ||
        host.find_first_of(" /\\\r\n\t") != std::string::npos) {
        if (error) *error = "Enter a hostname or IPv4 address only.";
        status_.last_error = error ? *error : "Invalid host.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (port < 1 || port > 65535) {
        if (error) *error = "Port must be between 1 and 65535.";
        status_.last_error = error ? *error : "Invalid port.";
        status_.last_result = "Mount rejected";
        return false;
    }
    if (remote_path.empty() || remote_path.size() > 1024 ||
        remote_path.find("..") != std::string::npos ||
        remote_path.find_first_of("?#\\\\\r\n") != std::string::npos) {
        if (error) *error = "Invalid remote path. '..', URL query characters, and line breaks are not allowed.";
        status_.last_error = error ? *error : "Invalid remote path.";
        status_.last_result = "Mount rejected";
        return false;
    }

    if (protocol == "nfs") {
        if (remote_path.front() != '/') {
            if (error) *error = "NFS requires the exact absolute export path, for example /mnt/dataPool/games.";
            status_.last_error = error ? *error : "Invalid NFS export path.";
            status_.last_result = "Mount rejected";
            return false;
        }
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
        if (error) *error = "This build has no PS5 FUSE/NFS adapter. Build with PS5NM_WITH_FUSE=1 and compatible ORBISDEV libraries.";
        status_.last_error = error ? *error : "NFS adapter not linked.";
        status_.last_result = "Mount not performed";
        return false;
#endif
    }

    if (remote_path.front() == '/' || remote_path.find("//") != std::string::npos) {
        if (error) *error = "SMB path must be ShareName or ShareName/subfolder, without a leading slash.";
        status_.last_error = error ? *error : "Invalid SMB share path.";
        status_.last_result = "Mount rejected";
        return false;
    }
#ifdef PS5NM_WITH_FUSE
    if (!StartSmbFuse(host, port, remote_path, mountpoint, username, password, error)) {
        status_.last_error = error ? *error : "SMB mount failed.";
        status_.last_result = "SMB mount failed";
        status_.mounted = false;
        return false;
    }
    status_.mounted = true;
    status_.protocol = "smb";
    status_.endpoint = "smb://" + host + ":" + std::to_string(port) + "/" + remote_path;
    status_.mountpoint = mountpoint;
    status_.last_result = "SMB share mounted read-only.";
    status_.last_error.clear();
    return true;
#else
    if (error) *error = "This build has no PS5 FUSE/NFS/SMB adapters linked. Build with PS5NM_WITH_FUSE=1 and compatible ORBISDEV libraries.";
    status_.last_error = error ? *error : "Filesystem adapters not linked.";
    status_.last_result = "Mount not performed";
    return false;
#endif
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
    if (status_.protocol == "smb" && (status_.mounted || SmbFuseIsMounted())) {
        if (!StopSmbFuse(mountpoint, error)) {
            status_.last_error = error ? *error : "SMB unmount failed.";
            status_.last_result = "Unmount failed";
            return false;
        }
        status_.mounted = false;
        status_.protocol.clear();
        status_.endpoint.clear();
        status_.last_result = "SMB share disconnected.";
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
