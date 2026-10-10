#include "mount_backend.hpp"

namespace ps5nm {

MountStatus MountBackend::Status() const {
    return status_;
}

bool MountBackend::Mount(const std::string& protocol, const std::string& host, int port,
                         const std::string& remote_path, const std::string& mountpoint,
                         std::string* error) {
    (void)host;
    (void)port;
    (void)remote_path;
    (void)mountpoint;
    if (protocol != "nfs" && protocol != "smb" && protocol != "ftp" && protocol != "sftp") {
        if (error) *error = "Unknown protocol. Choose NFS, SMB, FTP, or SFTP.";
        status_.last_error = error ? *error : "Unknown protocol.";
        status_.last_result = "Mount rejected";
        return false;
    }

    // Do not invoke shell commands or claim a mount until the target-specific
    // libfuse + libnfs adapter is integrated and mount-table verification exists.
    // ezRemote is a remote file client, not a filesystem mount implementation.
    if (error) {
        if (protocol == "nfs") {
            *error = "NFS mount adapter is not linked in this build. The PS5 libfuse/libnfs "
                     "adapter and mount verification must be integrated before mounting is enabled.";
        } else {
            *error = "This protocol has no filesystem-mount adapter yet. A remote file browser "
                     "connection is not a local mount.";
        }
    }
    status_.last_error = error ? *error : "Mount adapter unavailable.";
    status_.last_result = "Mount not performed";
    return false;
}

bool MountBackend::Unmount(const std::string& mountpoint, std::string* error) {
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages the /hostapp mountpoint.";
        return false;
    }
    if (!status_.mounted) {
        if (error) *error = "No mount owned by this payload is active.";
        status_.last_result = "Nothing to disconnect";
        return false;
    }
    // This branch is unreachable until a real adapter can create and track mounts.
    if (error) *error = "Unmount adapter is not available in this build.";
    return false;
}

} // namespace ps5nm
