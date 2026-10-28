#include "shortcut.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef PS5NM_HOME_BREW_DIR
#define PS5NM_HOME_BREW_DIR "/data/homebrew"
#endif

#ifdef PS5NM_EMBED_LAUNCHER_ELF
extern "C" {
extern const unsigned char ps5nm_launcher_start[];
extern const unsigned char ps5nm_launcher_end[];
}
#endif

namespace ps5nm {
namespace {
std::string ResolveHomebrewRoot() {
#ifndef PS5NM_EMBED_LAUNCHER_ELF
    const char* override_root = std::getenv("PS5NM_HOME_BREW_DIR");
    if (override_root && *override_root) return override_root;
#endif
    return PS5NM_HOME_BREW_DIR;
}

const std::string kHomebrewRoot = ResolveHomebrewRoot();
const std::string kTitleId = "HBRW00001";
std::mutex g_shortcut_lock;
constexpr unsigned int kShadowMountPort = 10101;
constexpr unsigned int kIconWidth = 512;
constexpr unsigned int kIconHeight = 512;
constexpr size_t kMaximumLauncherSize = 32 * 1024 * 1024;

uint32_t Crc32(const unsigned char* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

uint32_t Adler32(const unsigned char* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void AppendBigEndian(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>(value >> 24));
    out.push_back(static_cast<unsigned char>(value >> 16));
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void AppendPngChunk(std::vector<unsigned char>& out, const char type[4],
                    const std::vector<unsigned char>& data) {
    AppendBigEndian(out, static_cast<uint32_t>(data.size()));
    const size_t checksum_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    AppendBigEndian(out, Crc32(out.data() + checksum_start, 4 + data.size()));
}

std::vector<unsigned char> MakeIcon() {
    const size_t row_size = 1 + static_cast<size_t>(kIconWidth) * 4;
    std::vector<unsigned char> raw(row_size * kIconHeight);
    for (unsigned int y = 0; y < kIconHeight; ++y) {
        auto* row = raw.data() + static_cast<size_t>(y) * row_size;
        for (unsigned int x = 0; x < kIconWidth; ++x) {
            const size_t i = 1 + static_cast<size_t>(x) * 4;
            const bool light = ((x / 48) + (y / 48)) % 2 == 0;
            row[i] = light ? 35 : 20;
            row[i + 1] = light ? 115 : 48;
            row[i + 2] = light ? 220 : 100;
            row[i + 3] = 255;
        }
    }

    // PNG zlib stream using uncompressed DEFLATE blocks.
    std::vector<unsigned char> compressed{0x78, 0x01};
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t count = raw.size() - offset > 65535 ? 65535 : raw.size() - offset;
        compressed.push_back(offset + count == raw.size() ? 1 : 0);
        const uint16_t len = static_cast<uint16_t>(count);
        const uint16_t nlen = static_cast<uint16_t>(~len);
        compressed.push_back(static_cast<unsigned char>(len));
        compressed.push_back(static_cast<unsigned char>(len >> 8));
        compressed.push_back(static_cast<unsigned char>(nlen));
        compressed.push_back(static_cast<unsigned char>(nlen >> 8));
        compressed.insert(compressed.end(), raw.begin() + offset, raw.begin() + offset + count);
        offset += count;
    }
    AppendBigEndian(compressed, Adler32(raw.data(), raw.size()));

    std::vector<unsigned char> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<unsigned char> header;
    AppendBigEndian(header, kIconWidth);
    AppendBigEndian(header, kIconHeight);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    AppendPngChunk(png, "IHDR", header);
    AppendPngChunk(png, "IDAT", compressed);
    AppendPngChunk(png, "IEND", {});
    return png;
}

bool WriteAll(int fd, const unsigned char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool WriteFile(const std::string& path, const unsigned char* data, size_t size,
               mode_t mode, std::string* error) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        if (error) *error = "Cannot create " + path + ": " + std::strerror(errno);
        return false;
    }
    bool ok = WriteAll(fd, data, size);
    if (ok && fsync(fd) != 0) ok = false;
    int saved_errno = ok ? 0 : errno;
    if (close(fd) != 0) {
        if (ok) saved_errno = errno;
        ok = false;
    }
    if (!ok) {
        if (error) *error = "Cannot finish writing " + path + ": " + std::strerror(saved_errno);
        return false;
    }
    if (chmod(path.c_str(), mode) != 0) {
        if (error) *error = "Cannot set permissions on " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

#ifndef PS5NM_EMBED_LAUNCHER_ELF
bool CopyElf(const std::string& source, const std::string& destination, std::string* error) {
    const int input = open(source.c_str(), O_RDONLY);
    if (input < 0) {
        if (error) *error = "Cannot open the shortcut launcher ELF at " + source + ": " + std::strerror(errno);
        return false;
    }
    struct stat info{};
    if (fstat(input, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 64 ||
        static_cast<uint64_t>(info.st_size) > kMaximumLauncherSize) {
        close(input);
        if (error) *error = "The shortcut launcher must be a regular ELF file no larger than 32 MiB.";
        return false;
    }
    unsigned char header[20];
    if (read(input, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
        std::memcmp(header, "\x7f" "ELF", 4) != 0 || header[4] != 2 || header[5] != 1 ||
        header[18] != 0x3e || header[19] != 0) {
        close(input);
        if (error) *error = "The shortcut launcher must be a little-endian 64-bit x86-64 ELF executable.";
        return false;
    }
    if (lseek(input, 0, SEEK_SET) < 0) {
        close(input);
        if (error) *error = "Cannot rewind the shortcut launcher ELF.";
        return false;
    }
    const int output = open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (output < 0) {
        close(input);
        if (error) *error = "Cannot create the shortcut launcher: " + std::string(std::strerror(errno));
        return false;
    }
    unsigned char buffer[32768];
    bool ok = true;
    for (;;) {
        const ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        if (!WriteAll(output, buffer, static_cast<size_t>(count))) { ok = false; break; }
    }
    if (ok && fsync(output) != 0) ok = false;
    int saved_errno = ok ? 0 : errno;
    close(input);
    if (close(output) != 0) {
        if (ok) saved_errno = errno;
        ok = false;
    }
    if (!ok) {
        if (error) *error = "Cannot finish installing the shortcut launcher: " + std::string(std::strerror(saved_errno));
        return false;
    }
    if (chmod(destination.c_str(), 0755) != 0) {
        if (error) *error = "Cannot set permissions on the shortcut launcher: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}
#endif

void RemoveTree(const std::string& path) {
    struct stat root_info{};
    if (lstat(path.c_str(), &root_info) != 0) return;
    if (!S_ISDIR(root_info.st_mode)) {
        (void)unlink(path.c_str());
        return;
    }
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        const std::string child = path + "/" + entry->d_name;
        struct stat info{};
        if (lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) RemoveTree(child);
        else (void)unlink(child.c_str());
    }
    closedir(dir);
    (void)rmdir(path.c_str());
}

void RequestShadowMountScan(bool* detected) {
    if (detected) *detected = false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    timeval timeout{3, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(kShadowMountPort));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        static const char request[] =
            "POST /api/v1/scan HTTP/1.1\r\nHost: 127.0.0.1:10101\r\n"
            "Content-Type: application/json\r\nContent-Length: 2\r\n"
            "Connection: close\r\n\r\n{}";
        size_t sent = 0;
        while (sent < sizeof(request) - 1) {
            const ssize_t count = send(fd, request + sent, sizeof(request) - 1 - sent, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            sent += static_cast<size_t>(count);
        }
        if (sent == sizeof(request) - 1) {
            char response[1024];
            const ssize_t response_size = recv(fd, response, sizeof(response) - 1, 0);
            if (response_size > 0) {
                response[response_size] = '\0';
                if (std::strncmp(response, "HTTP/1.1 200", 12) == 0) {
                    const char* body = std::strstr(response, "\r\n\r\n");
                    if (body && std::strstr(body + 4, "\"queued\":true")) {
                        if (detected) *detected = true;
                    }
                }
            }
        }
    }
    close(fd);
}
} // namespace

ShortcutResult CreateHomeScreenShortcut(const std::string& launcher_elf) {
    std::lock_guard<std::mutex> lock(g_shortcut_lock);
    ShortcutResult result;
    const std::string install_root = kHomebrewRoot + "/" + kTitleId;
    result.path = install_root;
#ifndef PS5NM_EMBED_LAUNCHER_ELF
    if (launcher_elf.empty()) {
        result.http_status = 400;
        result.message = "This host test build has no bundled PS5 launcher ELF.";
        return result;
    }
    if (launcher_elf.size() > 1024 || launcher_elf.find_first_of("\r\n") != std::string::npos) {
        result.http_status = 400;
        result.message = "launcher_elf must be a valid filesystem path no longer than 1024 characters.";
        return result;
    }
#endif

    const std::string stage_root = kHomebrewRoot + "/.PSNM00001.installing." + std::to_string(getpid());
    const std::string backup = kHomebrewRoot + "/.PSNM00001.previous";
    struct stat info{};
    if (stat(kHomebrewRoot.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        result.http_status = 500;
        result.message = "Cannot access " + kHomebrewRoot + "; ensure PS5 homebrew storage is available.";
        return result;
    }
    if (mkdir(stage_root.c_str(), 0777) != 0) {
        result.http_status = 500;
        result.message = "Cannot prepare the shortcut folder: " + std::string(std::strerror(errno));
        return result;
    }
    const std::string sce_sys = stage_root + "/sce_sys";
    if (mkdir(sce_sys.c_str(), 0777) != 0 || chmod(stage_root.c_str(), 0777) != 0 || chmod(sce_sys.c_str(), 0777) != 0) {
        result.message = "Cannot create the shortcut metadata folder: " + std::string(std::strerror(errno));
        RemoveTree(stage_root);
        return result;
    }

#ifdef PS5NM_EMBED_LAUNCHER_ELF
    const size_t launcher_size = static_cast<size_t>(ps5nm_launcher_end - ps5nm_launcher_start);
    if (launcher_size < 64 || ps5nm_launcher_start[0] != 0x7f || ps5nm_launcher_start[1] != 'E' ||
        ps5nm_launcher_start[2] != 'L' || ps5nm_launcher_start[3] != 'F' ||
        ps5nm_launcher_start[4] != 2 || ps5nm_launcher_start[5] != 1 ||
        ps5nm_launcher_start[18] != 0x3e || ps5nm_launcher_start[19] != 0 ||
        !WriteFile(stage_root + "/eboot.bin", ps5nm_launcher_start, launcher_size, 0755, &result.message)) {
        if (result.message.empty()) result.message = "The bundled shortcut launcher is not an x86-64 PS5 ELF executable.";
        RemoveTree(stage_root);
        return result;
    }
#else
    if (!CopyElf(launcher_elf, stage_root + "/eboot.bin", &result.message)) {
        RemoveTree(stage_root);
        return result;
    }
#endif

    static const char param_json[] =
        "{\n"
        "  \"ageLevel\": {\"default\": 0},\n"
        "  \"applicationCategoryType\": 0,\n"
        "  \"applicationDrmType\": \"free\",\n"
        "  \"attribute\": 0,\n"
        "  \"conceptId\": \"00001\",\n"
        "  \"contentBadgeType\": 1,\n"
        "  \"contentId\": \"UP9000-HBRW00001_00-PS5NETMOUNT0001\",\n"
        "  \"contentVersion\": \"01.001.003\",\n"
        "  \"downloadDataSize\": 256,\n"
        "  \"gameIntent\": {\"permittedIntents\": [{\"intentType\": \"launchActivity\"}]},\n"
        "  \"localizedParameters\": {\"defaultLanguage\": \"en-US\", \"en-US\": {\"titleName\": \"PS5 Network Mounting\"}},\n"
        "  \"masterVersion\": \"01.00\",\n"
        "  \"requiredSystemSoftwareVersion\": \"0x0000000000000000\",\n"
        "  \"sdkVersion\": \"0x0000000000000000\",\n"
        "  \"titleId\": \"HBRW00001\",\n"
        "  \"versionFileUri\": \"\"\n"
        "}\n";
    if (!WriteFile(sce_sys + "/param.json", reinterpret_cast<const unsigned char*>(param_json), sizeof(param_json) - 1, 0644, &result.message)) {
        RemoveTree(stage_root);
        return result;
    }
    const auto icon = MakeIcon();
    if (!WriteFile(sce_sys + "/icon0.png", icon.data(), icon.size(), 0644, &result.message)) {
        RemoveTree(stage_root);
        return result;
    }

    if (lstat(backup.c_str(), &info) == 0) {
        result.message = "A previous shortcut backup already exists; refusing to delete it automatically.";
        RemoveTree(stage_root);
        return result;
    }
    if (errno != ENOENT) {
        result.message = "Cannot inspect the previous shortcut backup: " + std::string(std::strerror(errno));
        RemoveTree(stage_root);
        return result;
    }
    bool backed_up = false;
    if (lstat(install_root.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode) || rename(install_root.c_str(), backup.c_str()) != 0) {
            result.message = "Cannot safely replace the existing shortcut: " + std::string(std::strerror(errno));
            RemoveTree(stage_root);
            return result;
        }
        backed_up = true;
    }
    if (rename(stage_root.c_str(), install_root.c_str()) != 0) {
        const int saved_errno = errno;
        if (backed_up) (void)rename(backup.c_str(), install_root.c_str());
        result.message = "Cannot activate the home-screen shortcut: " + std::string(std::strerror(saved_errno));
        RemoveTree(stage_root);
        return result;
    }
    if (backed_up) RemoveTree(backup);

    RequestShadowMountScan(&result.shadow_mountplus_detected);
    result.success = true;
    result.http_status = 200;
    result.message = result.shadow_mountplus_detected
        ? "Shortcut installed; ShadowMountPlus was asked to scan. The tile should appear on the home screen shortly."
        : "Shortcut installed at " + install_root + ". ShadowMountPlus was not detected; launch it or wait for its next scan to register the home-screen tile.";
    return result;
}
} // namespace ps5nm
