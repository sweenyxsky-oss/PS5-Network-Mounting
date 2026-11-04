// Validation and orchestration for one mount request. The protocol libraries live in
// nfs_share.cpp and smb_share.cpp; nothing here knows how either of them works.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_backend.hpp"

#include <cerrno>
#include <cstring>
#include <memory>

#include "diagnostic_log.hpp"
#include "share.hpp"

namespace ps5nm {

MountStatus MountBackend::Status() const {
    MountStatus status = Vfs::Instance().Status();
    status.last_result = last_result_;
    status.last_error = last_error_;
    return status;
}

bool MountBackend::Mount(const std::string& protocol, const std::string& host, int port,
                         const std::string& remote_path, const std::string& mountpoint,
                         const std::string& username, const std::string& password,
                         std::string* error) {
    LogEvent("INFO", "mount", protocol + " request: " + host + ":" + std::to_string(port) +
                          remote_path + " -> " + mountpoint);

    if (protocol != "nfs" && protocol != "smb") {
        if (error) *error = "Unsupported protocol. Choose NFS or SMB.";
        last_error_ = error ? *error : "Unsupported protocol.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only serves the /hostapp root.";
        last_error_ = error ? *error : "Unsafe mountpoint.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (host.empty() || host.size() > 253 || host.find_first_of(" /\\\r\n\t") != std::string::npos) {
        if (error) *error = "Enter a hostname or IPv4 address only.";
        last_error_ = error ? *error : "Invalid host.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (port < 1 || port > 65535) {
        if (error) *error = "Port must be between 1 and 65535.";
        last_error_ = error ? *error : "Invalid port.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (remote_path.empty() || remote_path.size() > 1024 ||
        remote_path.find("..") != std::string::npos ||
        remote_path.find_first_of("?#\\\r\n") != std::string::npos) {
        if (error) *error = "Invalid remote path. '..', URL query characters and line breaks are not allowed.";
        last_error_ = error ? *error : "Invalid remote path.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (protocol == "nfs" && remote_path.front() != '/') {
        if (error) *error = "NFS requires the exact absolute export path, for example /mnt/dataPool/games.";
        last_error_ = error ? *error : "Invalid NFS export path.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (protocol == "smb" && !IsSafeRelativePath(remote_path)) {
        if (error) *error = "SMB path must be ShareName or ShareName/subfolder, without a leading slash.";
        last_error_ = error ? *error : "Invalid SMB share path.";
        last_result_ = "Mount rejected";
        return false;
    }
    if (Vfs::Instance().Attached()) {
        if (error) *error = "A share is already attached. Disconnect it before connecting another one.";
        last_error_ = error ? *error : "Another share is attached.";
        last_result_ = "Mount rejected";
        return false;
    }

    ShareSpec spec;
    spec.protocol = protocol == "smb" ? Protocol::kSmb : Protocol::kNfs;
    spec.host = host;
    spec.port = port;
    spec.remote_path = remote_path;
    spec.username = username;
    spec.password = password;
    spec.mountpoint = mountpoint;

#if defined(PS5NM_HOST)
    (void)spec;
    if (error) *error = "This host build has no share backends linked.";
    last_error_ = error ? *error : "No backends.";
    last_result_ = "Mount not performed";
    return false;
#else
    std::unique_ptr<ShareBackend> backend =
        spec.protocol == Protocol::kSmb ? MakeSmbBackend(spec) : MakeNfsBackend(spec);
    if (!backend) {
        if (error) *error = "Could not create the share backend.";
        last_error_ = error ? *error : "No backend.";
        last_result_ = "Mount not performed";
        return false;
    }

    ShareStatus status;
    if (!backend->Connect(&status)) {
        if (error) *error = status.message.empty() ? "The share could not be connected." : status.message;
        last_error_ = error ? *error : "Connection failed.";
        last_result_ = "Mount failed";
        LogEvent("ERROR", protocol, last_error_);
        return false;
    }

    std::string attach_error;
    if (!Vfs::Instance().Attach(std::move(backend), spec, &attach_error)) {
        if (error) *error = attach_error;
        last_error_ = attach_error;
        last_result_ = "Mount failed";
        LogEvent("ERROR", protocol, last_error_);
        return false;
    }

    last_error_.clear();
    last_result_ = std::string(protocol == "smb" ? "SMB" : "NFS") + " share attached read-only at " + mountpoint + ".";
    LogEvent("INFO", protocol, last_result_);
    return true;
#endif
}

bool MountBackend::Unmount(const std::string& mountpoint, std::string* error) {
    LogEvent("INFO", "mount", "Unmount requested for " + mountpoint);
    if (mountpoint != "/hostapp") {
        if (error) *error = "For safety, this payload only manages the /hostapp root.";
        last_error_ = error ? *error : "Unsafe mountpoint.";
        return false;
    }
    std::string detach_error;
    if (!Vfs::Instance().Detach(&detach_error)) {
        if (error) *error = detach_error;
        last_error_ = detach_error;
        last_result_ = "Nothing to disconnect";
        return false;
    }
    last_error_.clear();
    last_result_ = "Share disconnected.";
    return true;
}

}  // namespace ps5nm
