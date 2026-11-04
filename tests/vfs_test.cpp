// Host unit tests for path resolution, the listing cache and read-only enforcement.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "fake_backend.hpp"
#include "vfs.hpp"

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
}

template <typename T>
void CheckEqual(const T& actual, const T& expected, const char* what) {
    if (actual == expected) return;
    std::cerr << "FAIL: " << what << " (got " << actual << ", expected " << expected << ")\n";
    failures++;
}

}  // namespace

int main() {
    (void)setenv("PS5NM_LOG_DIR", "/tmp/ps5nm-vfs-test", 1);

    // Path helpers.
    std::vector<std::string> parts;
    Check(ps5nm::SplitPath("/a//b/./c/", &parts), "SplitPath accepts a normal path");
    Check(parts.size() == 3 && parts[0] == "a" && parts[1] == "b" && parts[2] == "c",
          "SplitPath drops empty and '.' components");
    Check(!ps5nm::SplitPath("/../etc", &parts), "SplitPath rejects climbing above the first component");
    Check(ps5nm::IsSafeRelativePath("games/PS5"), "IsSafeRelativePath accepts Share/subfolder");
    Check(!ps5nm::IsSafeRelativePath("/games"), "IsSafeRelativePath rejects a leading slash");
    Check(!ps5nm::IsSafeRelativePath("games/../etc"), "IsSafeRelativePath rejects dot-dot");
    CheckEqual(ps5nm::ParentPath("/hostapp/a.iso"), std::string("/hostapp"), "ParentPath");
    CheckEqual(ps5nm::BaseName("/hostapp/a.iso"), std::string("a.iso"), "BaseName");

    auto backend = std::make_unique<ps5nm::test::FakeBackend>();
    backend->AddFile("/games/game.iso", "0123456789");
    backend->AddFile("/games/sub/eboot.bin", "ELF");
    ps5nm::test::FakeBackend* fake = backend.get();

    std::string error;
    Check(ps5nm::Vfs::Instance().Attach(std::move(backend), ps5nm::ShareSpec{}, &error),
          "Attach accepts the fake backend");
    if (!error.empty()) std::fprintf(stderr, "attach error: %s\n", error.c_str());

    ps5nm::Vfs& vfs = ps5nm::Vfs::Instance();
    Check(vfs.IsManaged("/hostapp"), "the mount root is managed");
    Check(vfs.IsManaged("/hostapp/games/game.iso"), "a path below the root is managed");
    Check(!vfs.IsManaged("/hostappx/games"), "a sibling path is not managed");
    Check(!vfs.IsManaged("/etc/passwd"), "an unrelated path is not managed");
    Check(!vfs.IsManaged("/hostapp/../etc/passwd"), "a climbing path is not managed");

    struct stat st {};
    Check(vfs.Stat("/hostapp", &st, true) == 0 && S_ISDIR(st.st_mode), "the root stats as a folder");
    Check(vfs.Stat("/hostapp/games/game.iso", &st, true) == 0, "a file stats");
    CheckEqual(static_cast<long long>(st.st_size), 10LL, "stat reports the file size");
    Check((st.st_mode & 0555) == 0555, "files are mapped executable and read-only");
    Check(vfs.Stat("/hostapp/games/missing.iso", &st, true) == -1 && errno == ENOENT,
          "a missing file reports ENOENT");
    Check(vfs.Access("/hostapp/games/game.iso", W_OK) == -1 && errno == EROFS,
          "write access on a share is refused with EROFS");

    // A folder listing fills the cache, so the stat of a child needs no backend call.
    int open_errno = 0;
    std::unique_ptr<ps5nm::ShareDir> dir = vfs.OpenDir("/hostapp/games", &open_errno);
    Check(static_cast<bool>(dir), "the games folder opens");
    std::string name;
    ps5nm::NodeInfo info;
    ps5nm::ShareStatus status;
    int listed = 0;
    while (dir && dir->Next(&name, &info, &status)) listed++;
    CheckEqual(listed, 2, "the games folder lists its two entries");
    dir.reset();

    const int before = fake->stat_calls;
    Check(vfs.Stat("/hostapp/games/game.iso", &st, true) == 0, "a cached entry still stats");
    CheckEqual(fake->stat_calls, before, "a listed entry is answered from the cache");

    // Reading through an open file.
    std::unique_ptr<ps5nm::ShareFile> file = vfs.OpenFile("/hostapp/games/game.iso", O_RDONLY, &open_errno);
    Check(static_cast<bool>(file), "the file opens read-only");
    if (file) {
        char buffer[16] = {};
        ps5nm::ShareStatus read_status;
        const ssize_t got = file->Read(buffer, sizeof(buffer), 0, &read_status);
        CheckEqual(got, static_cast<ssize_t>(10), "the whole file reads");
        Check(std::string(buffer, static_cast<size_t>(got > 0 ? got : 0)) == "0123456789", "the bytes are right");
        CheckEqual(file->Info().size, static_cast<uint64_t>(10), "the handle reports its size");
    }
    std::unique_ptr<ps5nm::ShareFile> refused = vfs.OpenFile("/hostapp/games/game.iso", O_WRONLY, &open_errno);
    Check(!refused && open_errno == EROFS, "opening for write is refused with EROFS");
    refused = vfs.OpenFile("/hostapp/games", O_RDONLY, &open_errno);
    Check(!refused && open_errno == EISDIR, "opening a folder as a file reports EISDIR");

    ps5nm::MountStatus mount = vfs.Status();
    Check(mount.mounted && mount.protocol == "nfs", "the status reports the attached share");
    Check(mount.endpoint.find("nfs://") == 0, "the status reports an endpoint");

    Check(vfs.Attach(std::make_unique<ps5nm::test::FakeBackend>(), ps5nm::ShareSpec{}, &error) == false,
          "a second attach is refused");

    std::string detach_error;
    Check(vfs.Detach(&detach_error), "Detach succeeds");
    Check(!vfs.IsManaged("/hostapp/games/game.iso"), "nothing is managed after a detach");
    Check(!vfs.Detach(&detach_error), "a second detach is refused");

    if (failures == 0) std::printf("vfs tests: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
