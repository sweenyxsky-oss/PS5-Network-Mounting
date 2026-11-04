// An in-memory ShareBackend for the host tests: no protocol library, no network.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "share.hpp"

namespace ps5nm::test {

struct FakeNode {
    bool is_dir = false;
    std::string data;
    int64_t mtime = 1700000000;
};

class FakeBackend final : public ShareBackend {
public:
    FakeBackend() {
        nodes_["/"] = FakeNode{true, {}, 1700000000};
    }

    void AddDir(const std::string& path) { nodes_[Key(path)] = FakeNode{true, {}, 1700000000}; }

    void AddFile(const std::string& path, const std::string& data) {
        const std::string key = Key(path);
        const size_t slash = key.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string parent = slash == 0 ? "/" : key.substr(0, slash);
            nodes_[parent].is_dir = true;
        }
        FakeNode node;
        node.data = data;
        nodes_[key] = node;
    }

    int stat_calls = 0;
    int open_calls = 0;
    int readdir_calls = 0;

    Protocol Kind() const override { return Protocol::kNfs; }
    bool Connect(ShareStatus* status) override {
        if (!status) return false;
        connected_ = true;
        status->code = 0;
        return true;
    }
    void Disconnect() override { connected_ = false; }
    bool Connected() const override { return connected_; }
    std::string Describe() const override { return "fake share"; }

    bool Stat(const std::string& rel, bool, NodeInfo* out, ShareStatus* status) override {
        stat_calls++;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = nodes_.find(Key(rel));
        if (it == nodes_.end()) {
            status->Fail(ENOENT, "no such file or folder: " + rel);
            return false;
        }
        *out = ToNode(it->second, NameOf(rel));
        status->code = 0;
        return true;
    }

    std::unique_ptr<ShareFile> Open(const std::string& rel, ShareStatus* status) override {
        open_calls++;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = nodes_.find(Key(rel));
        if (it == nodes_.end()) {
            status->Fail(ENOENT, "no such file: " + rel);
            return nullptr;
        }
        if (it->second.is_dir) {
            status->Fail(EISDIR, rel + " is a folder");
            return nullptr;
        }
        status->code = 0;
        return std::unique_ptr<ShareFile>(new FakeFile(ToNode(it->second, NameOf(rel)), it->second.data));
    }

    std::unique_ptr<ShareDir> OpenDir(const std::string& rel, ShareStatus* status) override {
        readdir_calls++;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = Key(rel);
        const auto it = nodes_.find(key);
        if (it == nodes_.end() || !it->second.is_dir) {
            status->Fail(ENOTDIR, rel + " is not a folder");
            return nullptr;
        }
        std::vector<std::pair<std::string, NodeInfo>> entries;
        for (const auto& [path, node] : nodes_) {
            if (path == key || path == "/") continue;
            const std::string parent = key == "/" ? "/" : key;
            const size_t slash = path.find_last_of('/');
            const std::string owner = slash == 0 ? "/" : path.substr(0, slash);
            if (owner != parent) continue;
            entries.emplace_back(NameOf(path), ToNode(node, NameOf(path)));
        }
        status->code = 0;
        return std::unique_ptr<ShareDir>(new FakeDir(std::move(entries)));
    }

    bool Volume(const std::string&, VolumeInfo* out, ShareStatus* status) override {
        out->blocks = 1024;
        out->blocks_free = 512;
        out->blocks_available = 512;
        out->name_max = 255;
        status->code = 0;
        return true;
    }

private:
    class FakeFile final : public ShareFile {
    public:
        FakeFile(NodeInfo info, std::string data) : info_(info), data_(std::move(data)) {}

        ssize_t Read(void* dst, size_t count, uint64_t offset, ShareStatus* status) override {
            if (offset >= data_.size()) {
                status->code = 0;
                return 0;
            }
            const size_t available = data_.size() - static_cast<size_t>(offset);
            const size_t take = count < available ? count : available;
            std::char_traits<char>::copy(static_cast<char*>(dst), data_.data() + offset, take);
            status->code = 0;
            return static_cast<ssize_t>(take);
        }

        const NodeInfo& Info() const override { return info_; }

    private:
        NodeInfo info_;
        std::string data_;
    };

    class FakeDir final : public ShareDir {
    public:
        explicit FakeDir(std::vector<std::pair<std::string, NodeInfo>> entries)
            : entries_(std::move(entries)) {}

        bool Next(std::string* name, NodeInfo* info, ShareStatus* status) override {
            if (next_ >= entries_.size()) {
                status->code = 0;
                return false;
            }
            if (name) *name = entries_[next_].first;
            if (info) *info = entries_[next_].second;
            next_++;
            status->code = 0;
            return true;
        }

    private:
        std::vector<std::pair<std::string, NodeInfo>> entries_;
        size_t next_ = 0;
    };

    static std::string Key(const std::string& rel) {
        if (rel.empty() || rel == "/") return "/";
        std::vector<std::string> parts;
        SplitPath(rel, &parts);
        return JoinPath(parts);
    }

    static std::string NameOf(const std::string& rel) { return BaseName(Key(rel)); }

    static NodeInfo ToNode(const FakeNode& node, const std::string& name) {
        NodeInfo info;
        info.is_dir = node.is_dir;
        info.size = node.is_dir ? 0 : node.data.size();
        info.inode = static_cast<uint64_t>(std::hash<std::string>{}(name));
        info.nlink = node.is_dir ? 2 : 1;
        info.mode = node.is_dir ? 0755 : 0644;
        info.mtime = node.mtime;
        return info;
    }

    std::mutex mutex_;
    std::map<std::string, FakeNode> nodes_;
    bool connected_ = false;
};

}  // namespace ps5nm::test
