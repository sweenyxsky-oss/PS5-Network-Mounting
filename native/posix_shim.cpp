// The libc interposition layer: every path below the mount root is answered from the
// attached share, everything else goes to the real call untouched.
//
// The Makefile turns native/wrap.list into -Wl,--wrap=<name>, so each definition below
// pairs with a __real_<name> from the C library. Open files are kept in a fixed table
// of slots: a FILE* handed to the application points at our own FILE inside that table
// (never at one the C library created), and descriptors live far above any number the
// kernel hands out.
//
// Copyright (C) 2026 the PS5 Network Mounting contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "diagnostic_log.hpp"
#include "share.hpp"
#include "vfs.hpp"

extern "C" {
FILE* __real_fopen(const char*, const char*);
FILE* __real_freopen(const char*, const char*, FILE*);
int __real_fclose(FILE*);
size_t __real_fread(void*, size_t, size_t, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_fseek(FILE*, long, int);
int __real_fseeko(FILE*, off_t, int);
long __real_ftell(FILE*);
off_t __real_ftello(FILE*);
void __real_rewind(FILE*);
int __real_feof(FILE*);
int __real_ferror(FILE*);
int __real_fileno(FILE*);
int __real_fflush(FILE*);
int __real_fgetc(FILE*);
char* __real_fgets(char*, int, FILE*);
int __real_fputc(int, FILE*);
int __real_fputs(const char*, FILE*);
int __real_fprintf(FILE*, const char*, ...);
int __real_vfprintf(FILE*, const char*, va_list);
int __real_setvbuf(FILE*, char*, int, size_t);
void __real_flockfile(FILE*);
void __real_funlockfile(FILE*);
int __real_open(const char*, int, ...);
int __real_close(int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real_pread(int, void*, size_t, off_t);
off_t __real_lseek(int, off_t, int);
int __real_fstat(int, struct stat*);
int __real_stat(const char*, struct stat*);
int __real_lstat(const char*, struct stat*);
int __real_access(const char*, int);
char* __real_realpath(const char*, char*);
DIR* __real_opendir(const char*);
struct dirent* __real_readdir(DIR*);
int __real_closedir(DIR*);
}

namespace {

constexpr int kSlots = 64;
constexpr int kFdBase = 0x3d000000;  // far above any descriptor the kernel hands out

struct Slot {
    std::mutex mutex;
    std::unique_ptr<ps5nm::ShareFile> file;
    ps5nm::NodeInfo info;
    uint64_t pos = 0;
    bool eof = false;
    bool error = false;
    bool is_fd = false;
    bool active = true;
};

std::shared_ptr<Slot> g_slots[kSlots];
std::mutex g_slots_mutex;
FILE g_files[kSlots];  // the FILE objects we hand out; the C library never touches them

bool Managed(const char* path) {
    return path && ps5nm::Vfs::Instance().IsManaged(path);
}

int SlotOfFile(const FILE* fp) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(fp);
    const uintptr_t first = reinterpret_cast<uintptr_t>(&g_files[0]);
    if (value < first || value >= first + sizeof(g_files)) return -1;
    if ((value - first) % sizeof(FILE) != 0) return -1;
    return static_cast<int>((value - first) / sizeof(FILE));
}

int SlotOfFd(int fd) {
    return fd >= kFdBase && fd < kFdBase + kSlots ? fd - kFdBase : -1;
}

std::shared_ptr<Slot> LookupSlot(int index) {
    if (index < 0 || index >= kSlots) return nullptr;
    std::lock_guard<std::mutex> lock(g_slots_mutex);
    return g_slots[index];
}

int AcquireSlot(std::unique_ptr<ps5nm::ShareFile> file, bool as_fd) {
    if (!file) return -1;
    std::lock_guard<std::mutex> lock(g_slots_mutex);
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i]) continue;
        auto slot = std::make_shared<Slot>();
        slot->file = std::move(file);
        slot->info = slot->file->Info();
        slot->is_fd = as_fd;
        g_slots[i] = slot;
        std::memset(&g_files[i], 0, sizeof(FILE));
#if defined(__PROSPERO__) || defined(__ORBIS__)
        g_files[i]._flags = __SRD;
        g_files[i]._file = -1;
#endif
        return i;
    }
    return -1;
}

// Drops the slot from the table first, then waits for any read already in flight.
void ReleaseSlot(int index) {
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lock(g_slots_mutex);
        slot = std::move(g_slots[index]);
        g_slots[index].reset();
    }
    if (!slot) return;
    std::lock_guard<std::mutex> lock(slot->mutex);
    slot->active = false;
    slot->file.reset();
}

// The C library's own feof()/ferror() read these flags in C code; keep them in step.
void MirrorFlags(int index, const Slot& slot) {
#if defined(__PROSPERO__) || defined(__ORBIS__)
    FILE& fp = g_files[index];
    fp._flags = static_cast<short>((fp._flags & ~(__SEOF | __SERR)) |
                                   (slot.eof ? __SEOF : 0) | (slot.error ? __SERR : 0));
#else
    (void)index;
    (void)slot;
#endif
}

ssize_t ReadSlot(Slot& slot, void* dst, size_t count) {
    if (!slot.file || !slot.active) {
        errno = EBADF;
        return -1;
    }
    ps5nm::ShareStatus status;
    const ssize_t got = slot.file->Read(dst, count, slot.pos, &status);
    if (got < 0) {
        slot.error = true;
        errno = status.code ? status.code : EIO;
        return -1;
    }
    slot.pos += static_cast<uint64_t>(got);
    if (static_cast<size_t>(got) < count) slot.eof = true;
    return got;
}

bool SeekSlot(Slot& slot, int64_t offset, int whence) {
    int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(slot.pos);
    } else if (whence == SEEK_END) {
        base = static_cast<int64_t>(slot.info.size);
    } else if (whence != SEEK_SET) {
        errno = EINVAL;
        return false;
    }
    const int64_t target = base + offset;
    if (target < 0) {
        errno = EINVAL;
        return false;
    }
    slot.pos = static_cast<uint64_t>(target);
    slot.eof = false;
    return true;
}

bool ReadOnlyMode(const char* mode) {
    return mode && std::strpbrk(mode, "wa+") == nullptr;
}

// ---- directories ----
struct DirHandle {
    std::mutex mutex;
    std::unique_ptr<ps5nm::ShareDir> dir;
    struct dirent entry {};
};

std::mutex g_dirs_mutex;
std::vector<DirHandle*> g_dirs;

DirHandle* AsDir(DIR* raw) {
    DirHandle* handle = reinterpret_cast<DirHandle*>(raw);
    std::lock_guard<std::mutex> lock(g_dirs_mutex);
    for (DirHandle* candidate : g_dirs) {
        if (candidate == handle) return handle;
    }
    return nullptr;
}

}  // namespace

extern "C" {

FILE* __wrap_fopen(const char* path, const char* mode) {
    if (!Managed(path)) return __real_fopen(path, mode);
    if (!ReadOnlyMode(mode)) {
        errno = EROFS;
        return nullptr;
    }
    int open_errno = 0;
    std::unique_ptr<ps5nm::ShareFile> file = ps5nm::Vfs::Instance().OpenFile(path, O_RDONLY, &open_errno);
    if (!file) {
        errno = open_errno;
        return nullptr;
    }
    const int slot = AcquireSlot(std::move(file), false);
    if (slot < 0) {
        errno = EMFILE;
        return nullptr;
    }
    return &g_files[slot];
}

FILE* __wrap_freopen(const char* path, const char* mode, FILE* stream) {
    if (SlotOfFile(stream) >= 0 || Managed(path)) {
        errno = ENOTSUP;
        return nullptr;
    }
    return __real_freopen(path, mode, stream);
}

int __wrap_fclose(FILE* stream) {
    const int slot = SlotOfFile(stream);
    if (slot < 0) return __real_fclose(stream);
    ReleaseSlot(slot);
    return 0;
}

size_t __wrap_fread(void* dst, size_t size, size_t count, FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_fread(dst, size, count, stream);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot || size == 0 || count == 0) return 0;
    if (count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        return 0;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    const size_t wanted = size * count;
    const ssize_t got = ReadSlot(*slot, dst, wanted);
    MirrorFlags(index, *slot);
    if (got <= 0) return 0;
    return static_cast<size_t>(got) / size;
}

size_t __wrap_fwrite(const void* src, size_t size, size_t count, FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fwrite(src, size, count, stream);
    errno = EBADF;
    return 0;
}

int __wrap_fseeko(FILE* stream, off_t offset, int whence) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_fseeko(stream, offset, whence);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) {
        errno = EBADF;
        return -1;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    const bool ok = SeekSlot(*slot, static_cast<int64_t>(offset), whence);
    MirrorFlags(index, *slot);
    return ok ? 0 : -1;
}

int __wrap_fseek(FILE* stream, long offset, int whence) {
    if (SlotOfFile(stream) < 0) return __real_fseek(stream, offset, whence);
    return __wrap_fseeko(stream, static_cast<off_t>(offset), whence);
}

off_t __wrap_ftello(FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_ftello(stream);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) {
        errno = EBADF;
        return -1;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    return static_cast<off_t>(slot->pos);
}

long __wrap_ftell(FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_ftell(stream);
    const off_t position = __wrap_ftello(stream);
    if (position > static_cast<off_t>(LONG_MAX)) {
        errno = EOVERFLOW;
        return -1;
    }
    return static_cast<long>(position);
}

void __wrap_rewind(FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) {
        __real_rewind(stream);
        return;
    }
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) return;
    std::lock_guard<std::mutex> lock(slot->mutex);
    slot->pos = 0;
    slot->eof = false;
    slot->error = false;
    MirrorFlags(index, *slot);
}

int __wrap_feof(FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_feof(stream);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    return slot && slot->eof ? 1 : 0;
}

int __wrap_ferror(FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_ferror(stream);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    return slot && slot->error ? 1 : 0;
}

int __wrap_fileno(FILE* stream) {
    const int index = SlotOfFile(stream);
    if (index < 0) return __real_fileno(stream);
    return kFdBase + index;  // fstat() on it answers; close() on it does not close the FILE
}

int __wrap_fflush(FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fflush(stream);
    return 0;
}

int __wrap_setvbuf(FILE* stream, char* buffer, int mode, size_t size) {
    if (SlotOfFile(stream) < 0) return __real_setvbuf(stream, buffer, mode, size);
    return 0;
}

void __wrap_flockfile(FILE* stream) {
    if (SlotOfFile(stream) < 0) __real_flockfile(stream);
}

void __wrap_funlockfile(FILE* stream) {
    if (SlotOfFile(stream) < 0) __real_funlockfile(stream);
}

int __wrap_fgetc(FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fgetc(stream);
    unsigned char byte = 0;
    return __wrap_fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

char* __wrap_fgets(char* buffer, int size, FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fgets(buffer, size, stream);
    if (!buffer || size <= 0) {
        errno = EINVAL;
        return nullptr;
    }
    int written = 0;
    while (written < size - 1) {
        const int byte = __wrap_fgetc(stream);
        if (byte == EOF) break;
        buffer[written++] = static_cast<char>(byte);
        if (byte == '\n') break;
    }
    if (written == 0) return nullptr;
    buffer[written] = '\0';
    return buffer;
}

int __wrap_fputc(int byte, FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fputc(byte, stream);
    errno = EBADF;
    return EOF;
}

int __wrap_fputs(const char* text, FILE* stream) {
    if (SlotOfFile(stream) < 0) return __real_fputs(text, stream);
    errno = EBADF;
    return EOF;
}

int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
    if (SlotOfFile(stream) < 0) return __real_vfprintf(stream, format, args);
    errno = EBADF;
    return -1;
}

int __wrap_fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int __wrap_open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    if (!Managed(path)) return __real_open(path, flags, mode);
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        errno = EROFS;
        return -1;
    }
    if (flags & O_DIRECTORY) {
        errno = ENOTSUP;
        return -1;
    }
    int open_errno = 0;
    std::unique_ptr<ps5nm::ShareFile> file = ps5nm::Vfs::Instance().OpenFile(path, flags, &open_errno);
    if (!file) {
        errno = open_errno;
        return -1;
    }
    const int slot = AcquireSlot(std::move(file), true);
    if (slot < 0) {
        errno = EMFILE;
        return -1;
    }
    return kFdBase + slot;
}

int __wrap_close(int fd) {
    const int index = SlotOfFd(fd);
    if (index < 0) return __real_close(fd);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot || !slot->is_fd) {
        errno = EBADF;  // a FILE's number from fileno(): fclose() closes that
        return -1;
    }
    ReleaseSlot(index);
    return 0;
}

ssize_t __wrap_read(int fd, void* buffer, size_t count) {
    const int index = SlotOfFd(fd);
    if (index < 0) return __real_read(fd, buffer, count);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) {
        errno = EBADF;
        return -1;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    return ReadSlot(*slot, buffer, count);
}

ssize_t __wrap_pread(int fd, void* buffer, size_t count, off_t offset) {
    const int index = SlotOfFd(fd);
    if (index < 0) return __real_pread(fd, buffer, count, offset);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) {
        errno = EBADF;
        return -1;
    }
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    if (!slot->file || !slot->active) {
        errno = EBADF;
        return -1;
    }
    ps5nm::ShareStatus status;
    const ssize_t got = slot->file->Read(buffer, count, static_cast<uint64_t>(offset), &status);
    if (got < 0) {
        errno = status.code ? status.code : EIO;
        return -1;
    }
    return got;
}

off_t __wrap_lseek(int fd, off_t offset, int whence) {
    const int index = SlotOfFd(fd);
    if (index < 0) return __real_lseek(fd, offset, whence);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot) {
        errno = EBADF;
        return -1;
    }
    std::lock_guard<std::mutex> lock(slot->mutex);
    if (!SeekSlot(*slot, static_cast<int64_t>(offset), whence)) return -1;
    return static_cast<off_t>(slot->pos);
}

int __wrap_fstat(int fd, struct stat* out) {
    const int index = SlotOfFd(fd);
    if (index < 0) return __real_fstat(fd, out);
    const std::shared_ptr<Slot> slot = LookupSlot(index);
    if (!slot || !out) {
        errno = EBADF;
        return -1;
    }
    ps5nm::FillStatFromNode(slot->info, out);
    return 0;
}

int __wrap_stat(const char* path, struct stat* out) {
    if (!Managed(path)) return __real_stat(path, out);
    return ps5nm::Vfs::Instance().Stat(path, out, true);
}

int __wrap_lstat(const char* path, struct stat* out) {
    if (!Managed(path)) return __real_lstat(path, out);
    return ps5nm::Vfs::Instance().Stat(path, out, false);
}

int __wrap_access(const char* path, int mode) {
    if (!Managed(path)) return __real_access(path, mode);
    return ps5nm::Vfs::Instance().Access(path, mode);
}

char* __wrap_realpath(const char* path, char* resolved) {
    if (!Managed(path)) return __real_realpath(path, resolved);
    struct stat st {};
    if (ps5nm::Vfs::Instance().Stat(path, &st, true) != 0) {
        if (errno == 0) errno = ENOENT;
        return nullptr;
    }
    const std::string normal = ps5nm::Vfs::Instance().Normalise(path);
    if (normal.empty() || normal.size() >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    char* out = resolved ? resolved : static_cast<char*>(std::malloc(PATH_MAX));
    if (!out) {
        errno = ENOMEM;
        return nullptr;
    }
    std::memcpy(out, normal.c_str(), normal.size() + 1);
    return out;
}

DIR* __wrap_opendir(const char* path) {
    if (!Managed(path)) return __real_opendir(path);
    int open_errno = 0;
    std::unique_ptr<ps5nm::ShareDir> dir = ps5nm::Vfs::Instance().OpenDir(path, &open_errno);
    if (!dir) {
        errno = open_errno;
        return nullptr;
    }
    auto* handle = new DirHandle();
    handle->dir = std::move(dir);
    {
        std::lock_guard<std::mutex> lock(g_dirs_mutex);
        g_dirs.push_back(handle);
    }
    return reinterpret_cast<DIR*>(handle);
}

struct dirent* __wrap_readdir(DIR* raw) {
    DirHandle* handle = AsDir(raw);
    if (!handle) return __real_readdir(raw);
    std::lock_guard<std::mutex> lock(handle->mutex);
    std::string name;
    ps5nm::NodeInfo info;
    ps5nm::ShareStatus status;
    if (!handle->dir->Next(&name, &info, &status)) {
        if (!status.ok()) errno = status.code;
        return nullptr;
    }
    std::memset(&handle->entry, 0, sizeof(handle->entry));
    std::snprintf(handle->entry.d_name, sizeof(handle->entry.d_name), "%s", name.c_str());
    handle->entry.d_reclen = sizeof(handle->entry);
#if defined(__PROSPERO__) || defined(__ORBIS__) || defined(__FreeBSD__)
    handle->entry.d_fileno = static_cast<uint32_t>(info.inode ? info.inode : 1);
    handle->entry.d_namlen = static_cast<uint8_t>(std::strlen(handle->entry.d_name));
#else
    handle->entry.d_ino = static_cast<ino_t>(info.inode ? info.inode : 1);
#endif
    handle->entry.d_type = info.is_dir ? DT_DIR : (info.is_link ? DT_LNK : DT_REG);
    return &handle->entry;
}

int __wrap_closedir(DIR* raw) {
    DirHandle* handle = AsDir(raw);
    if (!handle) return __real_closedir(raw);
    {
        std::lock_guard<std::mutex> lock(g_dirs_mutex);
        for (auto it = g_dirs.begin(); it != g_dirs.end(); ++it) {
            if (*it == handle) {
                g_dirs.erase(it);
                break;
            }
        }
    }
    delete handle;
    return 0;
}

}  // extern "C"
