// The network-share layer of PS5 Network Mounting.
//
// A share is served in-process, the way PS5SX2 serves its NFS shares: the payload
// bundles the protocol client, linker-wraps the C library's file calls
// (native/posix_shim.cpp, native/wrap.list) and answers every path under the mount
// root itself. There is no kernel mount and no /dev/fuse.
//
// Both protocols sit behind ShareBackend, so the VFS, the shim, the HTTP API and the
// web panel are written once and do not know which one is attached.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps5nm {

enum class Protocol { kNfs, kSmb };

const char* ProtocolName(Protocol protocol);

// One validated request from the control panel.
struct ShareSpec {
    Protocol protocol = Protocol::kNfs;
    std::string host;
    int port = 0;
    std::string remote_path;  // NFS export path, or SMB "Share" / "Share/subfolder"
    std::string username;
    std::string password;
    std::string mountpoint = "/hostapp";
};

// A file or folder on a share, in the shape the POSIX layer needs.
struct NodeInfo {
    bool is_dir = false;
    bool is_link = false;
    uint64_t size = 0;
    uint64_t inode = 0;
    uint32_t nlink = 1;
    uint32_t mode = 0;  // the server's permission bits; the type comes from the flags above
    uint32_t uid = 0;
    uint32_t gid = 0;
    int64_t mtime = 0;
    uint32_t mtime_nsec = 0;
};

struct VolumeInfo {
    uint64_t block_size = 4096;
    uint64_t fragment_size = 4096;
    uint64_t blocks = 0;
    uint64_t blocks_free = 0;
    uint64_t blocks_available = 0;
    uint64_t files = 0;
    uint64_t files_free = 0;
    uint64_t name_max = 255;
};

// Result of one backend call: success, or a positive errno and a sentence for the log.
struct ShareStatus {
    int code = 0;
    std::string message;
    bool ok() const { return code == 0; }
    void Fail(int err, const std::string& text) {
        code = err > 0 ? err : EIO;
        message = text;
    }
};

// An open file. Reads are positional, so one handle is safe to share between threads
// as long as the owner serialises them (native/posix_shim.cpp does).
class ShareFile {
public:
    virtual ~ShareFile() = default;
    // >= 0 bytes read, or -1 with `status` set.
    virtual ssize_t Read(void* dst, size_t count, uint64_t offset, ShareStatus* status) = 0;
    virtual const NodeInfo& Info() const = 0;
};

// An open folder. Next() is called under the caller's lock.
class ShareDir {
public:
    virtual ~ShareDir() = default;
    // false with status->ok() means the directory ended.
    virtual bool Next(std::string* name, NodeInfo* info, ShareStatus* status) = 0;
};

class ShareBackend {
public:
    virtual ~ShareBackend() = default;

    virtual Protocol Kind() const = 0;
    // Connects and authenticates, and checks that the requested folder is readable.
    virtual bool Connect(ShareStatus* status) = 0;
    virtual void Disconnect() = 0;
    virtual bool Connected() const = 0;
    // One sentence for the panel, e.g. "NFS v3, export /volume1/PS2".
    virtual std::string Describe() const = 0;

    // `rel` is the path inside the share: "" for the share root, "/folder/file" below it.
    virtual bool Stat(const std::string& rel, bool follow, NodeInfo* out, ShareStatus* status) = 0;
    virtual std::unique_ptr<ShareFile> Open(const std::string& rel, ShareStatus* status) = 0;
    virtual std::unique_ptr<ShareDir> OpenDir(const std::string& rel, ShareStatus* status) = 0;
    virtual bool Volume(const std::string& rel, VolumeInfo* out, ShareStatus* status) = 0;
};

std::unique_ptr<ShareBackend> MakeNfsBackend(const ShareSpec& spec);
std::unique_ptr<ShareBackend> MakeSmbBackend(const ShareSpec& spec);

// ---- path helpers (shared by the backends, the VFS and the host tests) ----

// "/a//b/./c" -> {"a", "b", "c"}; false when ".." climbs above the first component.
bool SplitPath(const std::string& path, std::vector<std::string>* components);
std::string JoinPath(const std::vector<std::string>& components);
// A relative path from the panel: no "..", no backslashes, no line breaks.
bool IsSafeRelativePath(const std::string& path);
std::string ParentPath(const std::string& absolute_path);
std::string BaseName(const std::string& absolute_path);

}  // namespace ps5nm
