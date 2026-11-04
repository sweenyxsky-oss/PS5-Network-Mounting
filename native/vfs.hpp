// The in-process filesystem table: one attached share, the mount root it answers for,
// and the listing cache that turns a folder read into many stat() answers.
//
// Nothing here talks to a protocol library, which is what lets the host tests attach a
// fake backend and exercise the real resolution, caching and error paths.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "share.hpp"

namespace ps5nm {

struct MountStatus {
    bool mounted = false;
    bool can_disconnect = false;
    bool busy = false;  // open files keep the share attached to them after a detach
    std::string protocol;
    std::string endpoint;
    std::string mountpoint = "/hostapp";
    std::string backend_state;
    std::string last_result = "No operation yet";
    std::string last_error;
    uint64_t opens = 0;
    uint64_t reads = 0;
    uint64_t bytes = 0;
    uint64_t requests = 0;
    uint64_t cache_hits = 0;
    uint64_t open_files = 0;
};

class Vfs {
public:
    static Vfs& Instance();

    // Attaches a connected backend at `spec.mountpoint`. Verifies the share root first.
    bool Attach(std::unique_ptr<ShareBackend> backend, const ShareSpec& spec, std::string* error);
    // Detaches the share. Open handles stay readable until they are closed.
    bool Detach(std::string* error);
    bool Attached() const;

    MountStatus Status() const;

    // A path this layer answers for: the mount root itself or anything below it.
    bool IsManaged(const char* path) const;
    // Canonical form ("/hostapp/a/b"), or "" when the path climbs above the root.
    std::string Normalise(const char* path) const;
    std::string Root() const;

    // errno-style: 0 on success, -1 with errno set.
    int Stat(const char* path, struct stat* out, bool follow);
    int Access(const char* path, int mode);
    int Volume(const char* path, VolumeInfo* out);
    std::unique_ptr<ShareFile> OpenFile(const char* path, int flags, int* out_errno);
    std::unique_ptr<ShareDir> OpenDir(const char* path, int* out_errno);
    void NoteOpenFile();
    void NoteClosedFile();
    // Called by the directory recorder as a listing is consumed.
    void Remember(const std::string& folder, std::unordered_map<std::string, NodeInfo>&& names);

    // Test seam: attaches without a protocol backend factory.
    bool AttachForTest(std::unique_ptr<ShareBackend> backend, const std::string& mountpoint,
                       std::string* error);

private:
    Vfs() = default;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    struct Session {
        std::unique_ptr<ShareBackend> backend;
        ShareSpec spec;
        NodeInfo root;
        std::string endpoint;
        std::string state;
        std::atomic<bool> closing{false};
        std::mutex io;  // serialises calls into one protocol context
        ~Session();
    };

    struct Listing {
        double valid_until = 0;
        std::unordered_map<std::string, NodeInfo> names;
    };

    std::shared_ptr<Session> SessionForPath(const std::string& normal, std::string* rel) const;
    bool CachedStat(const std::string& normal, NodeInfo* out) const;

    mutable std::mutex mutex_;
    std::shared_ptr<Session> session_;
    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, Listing> listings_;
    std::atomic<uint64_t> opens_{0};
    std::atomic<uint64_t> reads_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> requests_{0};
    mutable std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> open_files_{0};
};

// Fills a struct stat from a node. Folders and files are read-only and executable
// (0555) so PS5 binaries and PRXs run straight off a share.
void FillStatFromNode(const NodeInfo& node, struct stat* out);

}  // namespace ps5nm
