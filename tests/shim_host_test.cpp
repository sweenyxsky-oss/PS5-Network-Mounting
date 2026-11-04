// Host tests for the libc interposition layer: the same --wrap list the PS5 build uses
// is linked here, so these calls go through native/posix_shim.cpp exactly as they will
// on the console.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "fake_backend.hpp"
#include "vfs.hpp"

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s (errno %d: %s)\n", what, errno, std::strerror(errno));
    failures++;
}

}  // namespace

int main() {
    (void)setenv("PS5NM_LOG_DIR", "/tmp/ps5nm-shim-test", 1);

    auto backend = std::make_unique<ps5nm::test::FakeBackend>();
    backend->AddFile("/games/game.iso", "0123456789");
    backend->AddFile("/games/sub/eboot.bin", "ELF");
    std::string error;
    if (!ps5nm::Vfs::Instance().Attach(std::move(backend), ps5nm::ShareSpec{}, &error)) {
        std::fprintf(stderr, "FAIL: could not attach the fake share: %s\n", error.c_str());
        return 1;
    }

    // stat()/lstat() are answered from the share.
    struct stat st {};
    Check(stat("/hostapp/games/game.iso", &st) == 0, "stat() on a share file");
    Check(st.st_size == 10, "stat() reports the size");
    Check(S_ISREG(st.st_mode), "stat() reports a regular file");
    Check((st.st_mode & 0777) == 0555, "stat() maps files read-only and executable");
    Check(lstat("/hostapp/games", &st) == 0 && S_ISDIR(st.st_mode), "lstat() on a share folder");
    Check(stat("/hostapp/games/nope", &st) == -1 && errno == ENOENT, "stat() on a missing file");
    Check(access("/hostapp/games/game.iso", R_OK) == 0, "access(R_OK) on a share file");
    Check(access("/hostapp/games/game.iso", W_OK) == -1 && errno == EROFS, "access(W_OK) is refused");

    // fopen()/fread()/fseek()/fclose() on a share file.
    std::FILE* file = fopen("/hostapp/games/game.iso", "rb");
    Check(file != nullptr, "fopen() on a share file");
    if (file) {
        char buffer[8] = {};
        Check(fread(buffer, 1, 4, file) == 4, "fread() reads four bytes");
        Check(std::memcmp(buffer, "0123", 4) == 0, "fread() returns the right bytes");
        Check(ftell(file) == 4, "ftell() follows the read");
        Check(fseek(file, 0, SEEK_END) == 0, "fseek() to the end");
        Check(ftell(file) == 10, "ftell() at the end");
        Check(feof(file) == 0, "feof() is clear after a seek");
        Check(fread(buffer, 1, 4, file) == 0, "fread() past the end returns 0");
        Check(feof(file) != 0, "feof() is set after the end");
        Check(fseek(file, 2, SEEK_SET) == 0, "fseek() back into the file");
        Check(fgetc(file) == '2', "fgetc() reads the next byte");
        Check(fclose(file) == 0, "fclose() on a share file");
    }
    errno = 0;
    Check(fopen("/hostapp/games/game.iso", "wb") == nullptr && errno == EROFS, "fopen() for write is refused");

    // open()/read()/pread()/lseek()/fstat()/close().
    const int fd = open("/hostapp/games/game.iso", O_RDONLY);
    Check(fd > 0, "open() on a share file");
    if (fd > 0) {
        char buffer[16] = {};
        Check(pread(fd, buffer, 4, 2) == 4, "pread() at an offset");
        Check(std::memcmp(buffer, "2345", 4) == 0, "pread() returns the right bytes");
        Check(read(fd, buffer, 3) == 3, "read() at the start");
        Check(std::memcmp(buffer, "012", 3) == 0, "read() returns the right bytes");
        Check(lseek(fd, 5, SEEK_SET) == 5, "lseek() moves the position");
        Check(fstat(fd, &st) == 0 && st.st_size == 10, "fstat() on a share descriptor");
        Check(close(fd) == 0, "close() on a share descriptor");
    }
    errno = 0;
    Check(open("/hostapp/games/game.iso", O_WRONLY) == -1 && errno == EROFS, "open() for write is refused");

    // opendir()/readdir()/closedir().
    DIR* dir = opendir("/hostapp/games");
    Check(dir != nullptr, "opendir() on a share folder");
    int seen = 0;
    bool saw_sub = false;
    if (dir) {
        while (struct dirent* entry = readdir(dir)) {
            seen++;
            if (std::strcmp(entry->d_name, "sub") == 0) {
                saw_sub = true;
                Check(entry->d_type == DT_DIR, "readdir() reports a folder entry");
            }
            if (std::strcmp(entry->d_name, "game.iso") == 0) {
                Check(entry->d_type == DT_REG, "readdir() reports a file entry");
            }
        }
        Check(closedir(dir) == 0, "closedir() on a share folder");
    }
    Check(seen == 2, "readdir() lists both entries");
    Check(saw_sub, "readdir() lists the subfolder");

    // realpath() and path normalisation.
    char resolved[PATH_MAX] = {};
    Check(realpath("/hostapp/games/../games/game.iso", resolved) != nullptr, "realpath() on a share path");
    Check(std::string(resolved) == "/hostapp/games/game.iso", "realpath() normalises the path");

    // Everything outside the mount root still reaches the kernel.
    const char* probe = "/tmp/ps5nm-shim-probe.txt";
    (void)unlink(probe);
    std::FILE* local = fopen(probe, "wb");
    Check(local != nullptr, "fopen() outside the root reaches the kernel");
    if (local) {
        Check(fwrite("kernel", 1, 5, local) == 5, "fwrite() outside the root reaches the kernel");
        Check(fclose(local) == 0, "fclose() outside the root");
    }
    std::FILE* read_back = fopen(probe, "rb");
    Check(read_back != nullptr, "the local file reopens");
    if (read_back) {
        char buffer[8] = {};
        Check(fread(buffer, 1, 5, read_back) == 5 && std::memcmp(buffer, "kernel", 5) == 0,
              "the local file reads back");
        Check(fclose(read_back) == 0, "the local file closes");
    }
    struct stat local_st {};
    Check(stat(probe, &local_st) == 0, "stat() outside the root reaches the kernel");
    Check(access(probe, F_OK) == 0, "access() outside the root reaches the kernel");
    (void)unlink(probe);

    std::string detach_error;
    Check(ps5nm::Vfs::Instance().Detach(&detach_error), "the fake share detaches");

    if (failures == 0) std::printf("shim tests: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
