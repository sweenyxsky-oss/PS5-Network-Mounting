#include "diagnostic_log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace ps5nm {
namespace {
constexpr size_t kMaxLogBytes = 512 * 1024;
constexpr size_t kSessionBufferBytes = 256 * 1024;
std::mutex g_mutex;
std::string g_path;
std::string g_session;
bool g_initialized = false;
bool g_persistent = false;

std::string DefaultPath() {
    const char* override_dir = std::getenv("PS5NM_LOG_DIR");
    const std::string dir = override_dir && *override_dir ? override_dir : "/data/PS5NetworkMounting";
    return dir + "/ps5nm.log";
}

bool MakeDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0775) == 0 || errno == EEXIST) {
        struct stat st{};
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    return false;
}

void RotateIfNeeded() {
    struct stat st{};
    if (stat(g_path.c_str(), &st) != 0 || st.st_size < static_cast<off_t>(kMaxLogBytes)) return;
    const std::string previous = g_path + ".1";
    (void)unlink(previous.c_str());
    (void)rename(g_path.c_str(), previous.c_str());
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (!localtime_r(&now, &tm)) return "unknown-time";
    char out[32];
    if (std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm) == 0) return "unknown-time";
    return out;
}

std::string Normalize(const char* value, const char* fallback) {
    std::string out = value && *value ? value : fallback;
    for (char& c : out) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (out.size() > 24) out.resize(24);
    return out;
}
} // namespace

bool InitDiagnosticLog() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return g_persistent;
    g_initialized = true;
    g_path = DefaultPath();
    const size_t slash = g_path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : g_path.substr(0, slash);
    if (MakeDirectory(dir)) {
        const int fd = open(g_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
        if (fd >= 0) {
            close(fd);
            g_persistent = true;
            RotateIfNeeded();
        }
    }
    return g_persistent;
}

void LogEvent(const char* level, const char* component, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        g_path = DefaultPath();
        const size_t slash = g_path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : g_path.substr(0, slash);
        if (MakeDirectory(dir)) {
            const int fd = open(g_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
            if (fd >= 0) { close(fd); g_persistent = true; }
        }
    }
    std::string line = "[" + Timestamp() + "] [" + Normalize(level, "INFO") + "] [" +
                       Normalize(component, "runtime") + "] " + message;
    for (char& c : line) if (c == '\r') c = ' ';
    line.push_back('\n');
    if (line.size() > 4096) line.resize(4096);

    if (g_session.size() + line.size() > kSessionBufferBytes) {
        const size_t excess = g_session.size() + line.size() - kSessionBufferBytes;
        const size_t next_line = g_session.find('\n', excess);
        g_session.erase(0, next_line == std::string::npos ? g_session.size() : next_line + 1);
    }
    g_session += line;
    if (g_persistent) {
        RotateIfNeeded();
        const int fd = open(g_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
        if (fd >= 0) {
            size_t written = 0;
            while (written < line.size()) {
                const ssize_t rc = write(fd, line.data() + written, line.size() - written);
                if (rc > 0) written += static_cast<size_t>(rc);
                else if (rc < 0 && errno == EINTR) continue;
                else break;
            }
            close(fd);
            if (written != line.size()) g_persistent = false;
        } else {
            g_persistent = false;
        }
    }
}

std::string ReadDiagnosticLog(size_t max_bytes, bool* persistent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (persistent) *persistent = g_persistent;
    std::string data;
    if (g_persistent && !g_path.empty()) {
        const int fd = open(g_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            struct stat st{};
            if (fstat(fd, &st) == 0 && st.st_size > 0) {
                const size_t size = static_cast<size_t>(st.st_size);
                const size_t start = size > max_bytes ? size - max_bytes : 0;
                data.resize(size - start);
                size_t read_at = 0;
                while (read_at < data.size()) {
                    const ssize_t rc = pread(fd, &data[read_at], data.size() - read_at, static_cast<off_t>(start + read_at));
                    if (rc > 0) read_at += static_cast<size_t>(rc);
                    else if (rc < 0 && errno == EINTR) continue;
                    else break;
                }
                data.resize(read_at);
            }
            close(fd);
        }
    }
    if (data.empty()) data = g_session;
    if (data.size() > max_bytes) data.erase(0, data.size() - max_bytes);
    return data;
}

std::string DiagnosticLogPath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) g_path = DefaultPath();
    return g_path;
}
} // namespace ps5nm
