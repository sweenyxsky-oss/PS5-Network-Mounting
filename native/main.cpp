#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <pthread.h>
#include <cstdio>
#include <map>
#include <sys/select.h>

#include "json.hpp"
#include "diagnostic_log.hpp"
#include "mount_backend.hpp"
#include "shortcut.hpp"
#include "web_assets.hpp"

extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool_id);

struct NotifyRequest {
    char reserved[45];
    char message[3075];
};
int sceKernelSendNotificationRequest(int, NotifyRequest*, unsigned long, int) __attribute__((weak));
int sceNotificationSend(int user_id, bool is_logged_in, const char* payload) __attribute__((weak));
int sceSystemServiceLaunchWebBrowser(const char* uri, void* reserved) __attribute__((weak));
int sceUserServiceInitialize(void*) __attribute__((weak));
int sceUserServiceTerminate(void) __attribute__((weak));
}

namespace {

#ifdef SYS_sendsyslog
#define PS5_SYS_SENDSYSLOG SYS_sendsyslog
#else
#define PS5_SYS_SENDSYSLOG 0x259
#endif

void LogKlog(const std::string &line) {
    std::string trimmed = line.size() > 400 ? line.substr(0, 400) + "..." : line;
    std::string msg = "<118>[PS5NM] " + trimmed + "\n";
#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(_ORBIS_)
    (void)syscall(PS5_SYS_SENDSYSLOG, 7, msg.c_str(), 0);
#endif
    std::fprintf(stdout, "[PS5NM] %s\n", trimmed.c_str());
    std::fflush(stdout);
    const bool is_error = line.find("failed") != std::string::npos || line.find("Error") != std::string::npos || line.find("error") != std::string::npos;
    const bool is_warning = line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos;
    const char* level = is_error ? "ERROR" : (is_warning ? "WARN" : "INFO");
    const char* component = line.find("[browser]") != std::string::npos ? "browser" :
        line.find("[notify]") != std::string::npos ? "notification" :
        line.find("[startup]") != std::string::npos ? "startup" : "payload";
    ps5nm::LogEvent(level, component, trimmed);
}

bool IsUsableLanAddress(const in_addr& address) {
    const uint32_t host = ntohl(address.s_addr);
    const unsigned int first = (host >> 24) & 0xff;
    const unsigned int second = (host >> 16) & 0xff;
    return host != INADDR_ANY && first != 0 && first != 127 && first < 224 &&
        !(first == 169 && second == 254);
}

std::vector<std::string> LocalIps() {
    std::vector<std::string> ips;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0 || !list) return ips;
    for (ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
            !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        const auto *addr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
        if (!IsUsableLanAddress(addr->sin_addr)) continue;
        char buf[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) continue;
        const std::string value(buf);
        if (std::find(ips.begin(), ips.end(), value) == ips.end()) ips.push_back(value);
    }
    freeifaddrs(list);
    return ips;
}

std::string LocalIp() {
    // Let the kernel select the address on the default route; UDP connect sends no packet.
    const int route_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (route_socket >= 0) {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(9);
        (void)inet_pton(AF_INET, "192.0.2.1", &destination.sin_addr);
        if (connect(route_socket, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0) {
            sockaddr_in local{};
            socklen_t size = sizeof(local);
            if (getsockname(route_socket, reinterpret_cast<sockaddr*>(&local), &size) == 0 &&
                IsUsableLanAddress(local.sin_addr)) {
                char buf[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                    close(route_socket);
                    return buf;
                }
            }
        }
        close(route_socket);
    }
    const auto ips = LocalIps();
    return ips.empty() ? "127.0.0.1" : ips.front();
}

bool InitUserService() {
    typedef int (*UserServiceInitFn)(void*);
    static UserServiceInitFn fn = nullptr;
    static bool resolved = false;
    static bool succeeded = false;
    if (succeeded) return true;
    if (!resolved) {
        resolved = true;
        if (sceUserServiceInitialize) {
            fn = sceUserServiceInitialize;
        } else {
            fn = reinterpret_cast<UserServiceInitFn>(dlsym(RTLD_DEFAULT, "sceUserServiceInitialize"));
            if (!fn) {
                void* handle = dlopen("libSceUserService.sprx", RTLD_LAZY);
                if (handle) {
                    fn = reinterpret_cast<UserServiceInitFn>(dlsym(handle, "sceUserServiceInitialize"));
                }
            }
        }
    }
    if (fn) {
        int rc = fn(nullptr);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "sceUserServiceInitialize rc=0x%08x (%d)", static_cast<unsigned>(rc), rc);
        LogKlog(buf);
        succeeded = (rc == 0 || rc == static_cast<int>(0x80550001));
        return succeeded;
    }
    LogKlog("sceUserServiceInitialize symbol not found (continuing)");
    return false;
}

bool SendPlainToastDirect(const std::string &message) {
    NotifyRequest req{};
    std::snprintf(req.message, sizeof(req.message), "%s", message.c_str());
    typedef int (*PlainNotifyFn)(int, NotifyRequest*, unsigned long, int);
    static PlainNotifyFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (sceKernelSendNotificationRequest) {
            fn = sceKernelSendNotificationRequest;
        } else {
            fn = reinterpret_cast<PlainNotifyFn>(dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest"));
        }
    }
    if (fn) {
        int rc = fn(0, &req, sizeof(req), 0);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[notify] sceKernelSendNotificationRequest rc=0x%08x (%d)",
                      static_cast<unsigned>(rc), rc);
        LogKlog(buf);
        return rc >= 0;
    }
    return false;
}

std::string JsonEscape(const std::string& value);

bool SendRichToastDirect(const std::string &message) {
    typedef int (*RichNotifyFn)(int, bool, const char*);
    static RichNotifyFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (sceNotificationSend) {
            fn = sceNotificationSend;
        } else {
            fn = reinterpret_cast<RichNotifyFn>(dlsym(RTLD_DEFAULT, "sceNotificationSend"));
            if (!fn) {
                void* handle = dlopen("libSceNotification.sprx", RTLD_LAZY);
                if (handle) {
                    fn = reinterpret_cast<RichNotifyFn>(dlsym(handle, "sceNotificationSend"));
                }
            }
        }
    }
    if (!fn) return false;

    std::string payload =
        "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"ServiceFeedback\",\"bundleName\":\"PS5 Network Mounting\","
        "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"InQueue\",\"isImmediate\":true,\"priority\":100,"
        "\"viewData\":{\"message\":{\"body\":\"" + JsonEscape(message) + "\"}}}}";
    constexpr int kSystemUserId = 0xFE;
    int rc = fn(kSystemUserId, true, payload.c_str());
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[notify] sceNotificationSend rc=0x%08x (%d)",
                  static_cast<unsigned>(rc), rc);
    LogKlog(buf);
    return rc == 0;
}

bool SendToastWithRetries(const std::string &message, int max_attempts = 5, int delay_ms = 500) {
    if (message.empty()) return false;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[notify] Attempt %d/%d sending toast: %s",
                      attempt, max_attempts, message.c_str());
        LogKlog(buf);

        if (SendPlainToastDirect(message)) {
            LogKlog("[notify] Toast delivered via sceKernelSendNotificationRequest");
            return true;
        }
        if (SendRichToastDirect(message)) {
            LogKlog("[notify] Toast delivered via sceNotificationSend");
            return true;
        }

        if (attempt < max_attempts) {
            usleep(delay_ms * 1000);
        }
    }
    LogKlog("[notify] All toast notification attempts exhausted");
    return false;
}

bool LaunchBrowserWithRetries(const std::string &url, int max_attempts = 5, int delay_ms = 1000) {
    if (url.empty()) return false;
    InitUserService();

    typedef int (*LaunchBrowserFn)(const char *uri, void *reserved);
    static LaunchBrowserFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (sceSystemServiceLaunchWebBrowser) {
            fn = sceSystemServiceLaunchWebBrowser;
        } else {
            fn = reinterpret_cast<LaunchBrowserFn>(dlsym(RTLD_DEFAULT, "sceSystemServiceLaunchWebBrowser"));
            if (!fn) {
                void *handle = dlopen("libSceSystemService.sprx", RTLD_LAZY);
                if (handle) {
                    fn = reinterpret_cast<LaunchBrowserFn>(dlsym(handle, "sceSystemServiceLaunchWebBrowser"));
                }
            }
        }
    }

    if (!fn) {
        LogKlog("[browser] sceSystemServiceLaunchWebBrowser symbol not found");
        return false;
    }

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        char buf[256];
        int rc = fn(url.c_str(), nullptr);
        std::snprintf(buf, sizeof(buf), "[browser] Attempt %d/%d: sceSystemServiceLaunchWebBrowser(\"%s\") rc=0x%08x (%d)",
                      attempt, max_attempts, url.c_str(), static_cast<unsigned>(rc), rc);
        LogKlog(buf);

        if (rc >= 0) {
            LogKlog("[browser] WebKit browser launched successfully");
            return true;
        }

        if (attempt < max_attempts) {
            usleep(delay_ms * 1000);
        }
    }

    LogKlog("[browser] All WebKit launch attempts exhausted");
    return false;
}

constexpr unsigned int kDefaultPort = 18080;
constexpr char kServiceName[] = "ps5-network-mounting";
constexpr char kVersion[] = "1.1.4";
constexpr int kNetHeapSize = 5 * 1024 * 1024;
constexpr size_t kMaxBody = 64 * 1024;
constexpr size_t kMaxHeader = 16 * 1024;
constexpr int kMaxConcurrentClients = 32;

ps5nm::MountBackend g_mount;
pthread_mutex_t g_mount_api_lock = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> g_browser_launched{false};
volatile sig_atomic_t g_server_socket = -1;
volatile sig_atomic_t g_stop_requested = 0;
unsigned int g_server_port = kDefaultPort;
std::string g_server_bind = "0.0.0.0";
std::string g_server_url = "http://127.0.0.1:18080";
pthread_mutex_t g_client_count_lock = PTHREAD_MUTEX_INITIALIZER;
int g_active_clients = 0;

struct HttpResponse {
    unsigned int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::string extra_headers;
};

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: if (c >= 0x20) out += static_cast<char>(c);
        }
    }
    return out;
}

std::string Quote(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

std::string StatusJson() {
    pthread_mutex_lock(&g_mount_api_lock);
    const auto s = g_mount.Status();
    pthread_mutex_unlock(&g_mount_api_lock);

    const auto ips = g_server_bind == "0.0.0.0"
        ? LocalIps() : std::vector<std::string>{g_server_bind};
    std::string urls = "[";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i) urls += ",";
        urls += Quote("http://" + ips[i] + ":" + std::to_string(g_server_port));
    }
    urls += "]";
    return "{\"state\":\"ready\",\"service\":" + Quote(kServiceName) +
        ",\"version\":" + Quote(kVersion) + ",\"port\":" + std::to_string(g_server_port) +
        ",\"url\":" + Quote(g_server_url) + ",\"urls\":" + urls +
        ",\"browser_launched\":" + std::string(g_browser_launched.load() ? "true" : "false") +
        ",\"mounted\":" + std::string(s.mounted ? "true" : "false") +
        ",\"can_disconnect\":" + std::string(s.can_disconnect ? "true" : "false") +
        ",\"protocol\":" + (s.protocol.empty() ? "null" : Quote(s.protocol)) +
        ",\"endpoint\":" + (s.endpoint.empty() ? "null" : Quote(s.endpoint)) +
        ",\"mountpoint\":" + Quote(s.mountpoint) + ",\"last_result\":" + Quote(s.last_result) +
        ",\"last_error\":" + (s.last_error.empty() ? "null" : Quote(s.last_error)) + "}";
}

HttpResponse ApiError(unsigned int status, const char* code, const std::string& message) {
    if (status >= 400) ps5nm::LogEvent("ERROR", "http", std::string(code ? code : "api_error") + ": " + message);
    return {status, "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":" + Quote(code) + ",\"message\":" + Quote(message) + "}", ""};
}

HttpResponse HandleRequest(const std::string& method, const std::string& raw_path,
                           const std::map<std::string, std::string>& headers,
                           const std::string& body) {
    std::string path = raw_path;
    const auto query = path.find('?');
    if (query != std::string::npos) path.resize(query);

    // CORS preflight support
    if (method == "OPTIONS") {
        return {204, "text/plain", "",
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type, X-Requested-With\r\n"
                "Access-Control-Max-Age: 86400"};
    }

    if (method == "GET" && (path == "/" || path == "/index.html"))
        return {200, "text/html; charset=utf-8", std::string(ps5nm::kIndexHtml), ""};

    if (method == "GET" && path == "/api/v1/status")
        return {200, "application/json; charset=utf-8", StatusJson(), "Access-Control-Allow-Origin: *"};

    if (method == "GET" && path == "/api/v1/log") {
        bool persistent = false;
        const std::string log = ps5nm::ReadDiagnosticLog(256 * 1024, &persistent);
        const std::string headers = std::string("Content-Disposition: attachment; filename=\"ps5nm.log\"\r\nX-PS5NM-Log-Persistent: ") +
            (persistent ? "available" : "session-only") + "\r\n";
        return {200, "text/plain; charset=utf-8", log.empty() ? "PS5 Network Mounting: no diagnostic events recorded yet.\n" : log, headers};
    }

    // Reject browser state changes from foreign origins. A fixed custom header is
    // not a CSRF token: an attacker can request it through a CORS preflight.
    if (method == "POST") {
        const auto origin_it = headers.find("origin");
        if (origin_it != headers.end()) {
            const auto host_it = headers.find("host");
            std::string origin = origin_it->second;
            const size_t scheme = origin.find("://");
            if (scheme != std::string::npos) origin.erase(0, scheme + 3);
            const size_t path = origin.find_first_of("/?#");
            if (path != std::string::npos) origin.resize(path);
            if (host_it == headers.end() || origin.empty() || origin != host_it->second)
                return ApiError(403, "forbidden_origin", "Cross-origin state-changing requests are not allowed.");
        }
    }

    if (method == "POST" && path == "/api/v1/mount") {
        ps5nm::JsonValue req;
        std::string json_err;
        if (!ps5nm::JsonValue::Parse(body, req, &json_err) || !req.IsObject()) {
            return ApiError(400, "invalid_json", "Malformed JSON body: " + json_err);
        }

        std::string protocol = req.GetString("protocol");
        std::string host = req.GetString("host");
        std::string remote = req.GetString("remote_path");
        std::string mountpoint = req.GetString("mountpoint");
        std::string username = req.GetString("username");
        std::string password = req.GetString("password");
        int port = req.Has("port") ? req.GetInt("port", 0) : (protocol == "smb" ? 445 : 2049);

        if (protocol.empty() || host.empty() || remote.empty() || mountpoint.empty()) {
            return ApiError(400, "invalid_request",
                            "Required fields are missing or malformed: protocol, host, remote_path, mountpoint.");
        }

        if (protocol != "nfs" && protocol != "smb")
            return ApiError(400, "unsupported_protocol", "Choose NFS or SMB.");

        if (port < 1 || port > 65535)
            return ApiError(400, "invalid_port", "Port must be an integer from 1 to 65535.");

        if (host.size() > 253 || host.find_first_of(" /\\\r\n\t") != std::string::npos)
            return ApiError(400, "invalid_host", "Enter a hostname or IPv4 address only.");
        if (remote.size() > 1024 ||
            remote.find("..") != std::string::npos ||
            remote.find_first_of("\r\n?#") != std::string::npos)
            return ApiError(400, "invalid_remote_path", "Remote path is empty, too long, or contains unsafe characters.");
        if (protocol == "nfs" && remote.front() != '/')
            return ApiError(400, "invalid_remote_path", "NFS requires an absolute export path, for example /mnt/dataPool/games.");
        if (protocol == "smb" && remote.front() == '/')
            return ApiError(400, "invalid_remote_path", "SMB path must be ShareName or ShareName/subfolder without a leading slash.");
        if (mountpoint != "/hostapp")
            return ApiError(400, "unsafe_mountpoint", "For safety, the only allowed mountpoint is /hostapp.");

        std::string error;
        pthread_mutex_lock(&g_mount_api_lock);
        const bool mounted = g_mount.Mount(protocol, host, port, remote, mountpoint,
                                           username, password, &error);
        pthread_mutex_unlock(&g_mount_api_lock);
        if (!mounted) return ApiError(500, "mount_failed", error);
        return {200, "application/json; charset=utf-8", StatusJson(), "Access-Control-Allow-Origin: *"};
    }

    if (method == "POST" && path == "/api/v1/browser") {
        ps5nm::JsonValue req;
        std::string url = g_server_url;
        if (!body.empty() && (!ps5nm::JsonValue::Parse(body, req) || !req.IsObject()))
            return ApiError(400, "invalid_json", "Browser request must be a JSON object.");
        if (req.IsObject() && req.Has("url")) url = req.GetString("url");
        if (url.size() > 2048 || (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) ||
            url.find_first_of("\r\n") != std::string::npos)
            return ApiError(400, "invalid_url", "Browser URL must be an HTTP or HTTPS URL without line breaks.");
        const bool launched = LaunchBrowserWithRetries(url, 3, 500);
        g_browser_launched.store(launched);
        if (!launched)
            return ApiError(502, "browser_launch_failed", "PS5 WebKit could not open " + url + ". Open this URL manually in the console browser.");
        return {200, "application/json; charset=utf-8",
                "{\"ok\":true,\"url\":" + Quote(url) + "}", "Access-Control-Allow-Origin: *"};
    }

    if (method == "POST" && path == "/api/v1/shortcut") {
        ps5nm::JsonValue req;
        if (!body.empty() && (!ps5nm::JsonValue::Parse(body, req) || !req.IsObject()))
            return ApiError(400, "invalid_json", "Shortcut request must be a JSON object.");
        std::string launcher;
#ifndef PS5NM_EMBED_LAUNCHER_ELF
        launcher = req.IsObject() ? req.GetString("launcher_elf") : std::string();
        if (launcher.empty()) {
            const char* configured = std::getenv("PS5NM_LAUNCHER_ELF");
            if (configured) launcher = configured;
        }
#endif
        const auto created = ps5nm::CreateHomeScreenShortcut(launcher);
        const std::string response = "{\"ok\":" + std::string(created.success ? "true" : "false") +
            ",\"path\":" + Quote(created.path) + ",\"shadow_mountplus_detected\":" +
            std::string(created.shadow_mountplus_detected ? "true" : "false") +
            ",\"message\":" + Quote(created.message) + "}";
        return {created.http_status, "application/json; charset=utf-8", response,
                "Access-Control-Allow-Origin: *"};
    }

    if (method == "POST" && path == "/api/v1/notify") {
        ps5nm::JsonValue req;
        std::string msg = "PS5 Network Mounting";
        if (ps5nm::JsonValue::Parse(body, req) && req.IsObject() && req.Has("message")) {
            msg = req.GetString("message");
        }
        const bool sent = SendToastWithRetries(msg, 3, 300);
        return {200, "application/json; charset=utf-8",
                std::string("{\"ok\":") + (sent ? "true" : "false") + ",\"message\":" + Quote(msg) + "}", "Access-Control-Allow-Origin: *"};
    }

    if (method == "POST" && path == "/api/v1/unmount") {
        ps5nm::JsonValue req;
        std::string json_err;
        if (!ps5nm::JsonValue::Parse(body, req, &json_err) || !req.IsObject()) {
            return ApiError(400, "invalid_json", "Malformed JSON body: " + json_err);
        }
        std::string mountpoint = req.GetString("mountpoint");
        if (mountpoint.empty())
            return ApiError(400, "invalid_request", "mountpoint is required.");
        std::string error;
        pthread_mutex_lock(&g_mount_api_lock);
        const bool unmounted = g_mount.Unmount(mountpoint, &error);
        pthread_mutex_unlock(&g_mount_api_lock);
        if (!unmounted) return ApiError(409, "unmount_failed", error);
        return {200, "application/json; charset=utf-8", StatusJson(), "Access-Control-Allow-Origin: *"};
    }

    return ApiError(404, "not_found", "Unknown page or API endpoint.");
}

const char* Reason(unsigned int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

bool SendAll(int fd, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const ssize_t n = send(fd, data + sent, size - sent, 0);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void SendResponse(int fd, const HttpResponse& response) {
    std::string header = std::string("HTTP/1.1 ") + std::to_string(response.status) +
        " " + Reason(response.status) + "\r\nContent-Type: " + response.content_type +
        "\r\nContent-Length: " + std::to_string(response.body.size()) +
        "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
    if (!response.extra_headers.empty()) {
        header += response.extra_headers;
        if (header.back() != '\n') header += "\r\n";
    }
    header += "\r\n";
    (void)SendAll(fd, header.data(), header.size());
    if (!response.body.empty()) {
        (void)SendAll(fd, response.body.data(), response.body.size());
    }
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void HandleClient(int fd) {
    timeval timeout{10, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string data;
    char buffer[4096];
    size_t split = std::string::npos;
    while ((split = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() >= kMaxHeader) {
            SendResponse(fd, ApiError(431, "headers_too_large", "HTTP headers exceed the 16 KiB limit."));
            return;
        }
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<size_t>(n));
    }
    if (split > kMaxHeader) {
        SendResponse(fd, ApiError(431, "headers_too_large", "HTTP headers exceed the 16 KiB limit."));
        return;
    }

    const std::string header = data.substr(0, split);
    std::string body = data.substr(split + 4);
    const auto line_end = header.find("\r\n");
    const std::string request_line = header.substr(0, line_end);
    const auto sp1 = request_line.find(' ');
    const auto sp2 = sp1 == std::string::npos ? sp1 : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        SendResponse(fd, ApiError(400, "bad_request", "Malformed HTTP request line."));
        return;
    }
    const std::string method = request_line.substr(0, sp1);
    const std::string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = request_line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        SendResponse(fd, ApiError(400, "bad_version", "Only HTTP/1.0 and HTTP/1.1 are supported."));
        return;
    }

    std::map<std::string, std::string> parsed_headers;
    size_t content_length = 0;
    bool has_content_length = false;
    size_t pos = line_end == std::string::npos ? header.size() : line_end + 2;
    while (pos < header.size()) {
        const auto end = header.find("\r\n", pos);
        const std::string line = header.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            SendResponse(fd, ApiError(400, "bad_header", "Malformed HTTP header."));
            return;
        }
        const std::string key = Lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        parsed_headers[key] = value;

        if (key == "content-length") {
            if (has_content_length) {
                SendResponse(fd, ApiError(400, "duplicate_length", "Duplicate Content-Length headers are not accepted."));
                return;
            }
            char* tail = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &tail, 10);
            if (tail == value.c_str() || *tail != '\0') {
                SendResponse(fd, ApiError(400, "bad_length", "Invalid Content-Length header."));
                return;
            }
            content_length = static_cast<size_t>(parsed);
            has_content_length = true;
        } else if (key == "transfer-encoding" && Lower(value) != "identity") {
            SendResponse(fd, ApiError(400, "unsupported_encoding", "Chunked request bodies are not supported."));
            return;
        }
        if (end == std::string::npos) break;
        pos = end + 2;
    }

    if (content_length > kMaxBody) {
        SendResponse(fd, ApiError(413, "body_too_large", "Request body exceeds the 64 KiB limit."));
        return;
    }
    if (body.size() > content_length) body.resize(content_length);
    while (body.size() < content_length) {
        const size_t want = content_length - body.size() < sizeof(buffer) ?
            content_length - body.size() : sizeof(buffer);
        const ssize_t n = recv(fd, buffer, want, 0);
        if (n <= 0) {
            SendResponse(fd, ApiError(400, "incomplete_body", "Request body ended before Content-Length bytes arrived."));
            return;
        }
        body.append(buffer, static_cast<size_t>(n));
    }

    SendResponse(fd, HandleRequest(method, path, parsed_headers, body));
}

void* ClientWorker(void* arg) {
    int fd = *static_cast<int*>(arg);
    delete static_cast<int*>(arg);
    HandleClient(fd);
    close(fd);

    pthread_mutex_lock(&g_client_count_lock);
    --g_active_clients;
    pthread_mutex_unlock(&g_client_count_lock);
    return nullptr;
}
}

struct StartupWorkerArgs {
    std::string web_url;
    std::string toast_message;
};

void StopSignalHandler(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

bool ProbeRunningService(const char* address, unsigned int port, std::string* url) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    timeval timeout{2, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        close(fd);
        return false;
    }
    static const char request[] = "GET /api/v1/status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < sizeof(request) - 1) {
        const ssize_t count = send(fd, request + sent, sizeof(request) - 1 - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[2048];
    for (;;) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        response.append(buffer, static_cast<size_t>(count));
        if (response.size() > 16384) break;
    }
    close(fd);
    const auto body_pos = response.find("\r\n\r\n");
    if (response.compare(0, 12, "HTTP/1.1 200") != 0 || body_pos == std::string::npos ||
        response.find("\"service\":\"ps5-network-mounting\"", body_pos + 4) == std::string::npos)
        return false;
    if (url) {
        const std::string marker = "\"url\":\"";
        const auto start = response.find(marker, body_pos + 4);
        if (start != std::string::npos) {
            const auto value_start = start + marker.size();
            const auto end = response.find('\"', value_start);
            if (end != std::string::npos) *url = response.substr(value_start, end - value_start);
        }
    }
    return true;
}

void* StartupWorker(void* arg) {
    auto* args = reinterpret_cast<StartupWorkerArgs*>(arg);
    LogKlog("[startup] Background startup worker began; waiting 300ms for system services to settle");
    usleep(300 * 1000);

    LogKlog("[startup] Sending startup notification toast");
    SendToastWithRetries(args->toast_message, 5, 500);

    if (std::getenv("PS5NM_NO_BROWSER") == nullptr || std::strcmp(std::getenv("PS5NM_NO_BROWSER"), "1") != 0) {
        LogKlog("[startup] Launching WebKit browser pointing to " + args->web_url);
        g_browser_launched.store(LaunchBrowserWithRetries(args->web_url, 5, 1000));
    } else {
        LogKlog("[startup] Automatic browser launch disabled by PS5NM_NO_BROWSER=1");
    }

    delete args;
    return nullptr;
}

int main() {
    (void)ps5nm::InitDiagnosticLog();
    LogKlog("Initializing PS5 Network Mounting payload");

    int net_init_rc = sceNetInit();
    if (net_init_rc != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "sceNetInit returned %d (non-fatal, continuing)", net_init_rc);
        LogKlog(buf);
    }
    const int net_pool = sceNetPoolCreate("ps5-network-mounting", kNetHeapSize, 0);
    if (net_pool < 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "sceNetPoolCreate returned %d (non-fatal, continuing)", net_pool);
        LogKlog(buf);
    }

    InitUserService();

    unsigned int port = kDefaultPort;
    if (const char* env = std::getenv("PS5NM_PORT")) {
        char* end = nullptr;
        const long value = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && value >= 1024 && value <= 65535)
            port = static_cast<unsigned int>(value);
    }

    // Default to 0.0.0.0 (all interfaces) so the UI is accessible via PS5 web browser
    // and from any PC/phone on the LAN using the PS5 IP address.
    const char* bind_ip = std::getenv("PS5NM_BIND");
    if (!bind_ip || !*bind_ip) bind_ip = "0.0.0.0";
    g_server_port = port;
    g_server_bind = bind_ip;
    const std::string local_ip = g_server_bind == "0.0.0.0" ? LocalIp() : g_server_bind;
    const std::string web_url = "http://" + local_ip + ":" + std::to_string(port);
    g_server_url = web_url;

    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGTERM, StopSignalHandler);
    (void)signal(SIGINT, StopSignalHandler);
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "socket creation failed (errno %d: %s)", errno, std::strerror(errno));
        LogKlog(buf);
        if (net_pool >= 0) (void)sceNetPoolDestroy(net_pool);
        return 2;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_NOSIGPIPE
    (void)setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        LogKlog("inet_pton failed for bind_ip=" + std::string(bind_ip));
        close(server);
        if (net_pool >= 0) (void)sceNetPoolDestroy(net_pool);
        return 3;
    }

    bool bound = false;
    constexpr int kMaxBindAttempts = 10;
    for (int attempt = 1; attempt <= kMaxBindAttempts; ++attempt) {
        if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            listen(server, 16) == 0) {
            bound = true;
            break;
        }

        const int bind_err = errno;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "bind/listen attempt %d/%d on %s:%u failed (errno %d: %s)",
                      attempt, kMaxBindAttempts, bind_ip, port, bind_err, std::strerror(bind_err));
        LogKlog(buf);

        if (attempt < kMaxBindAttempts && (bind_err == EADDRINUSE || bind_err == EADDRNOTAVAIL))
            usleep(500 * 1000);
        else if (bind_err != EADDRINUSE && bind_err != EADDRNOTAVAIL)
            break;
    }

    if (!bound) {
        LogKlog("bind/listen failed permanently on " + std::string(bind_ip) + ":" + std::to_string(port));
        close(server);

        std::string active_url = web_url;
        const auto ips = LocalIps();
        bool already_running = ProbeRunningService("127.0.0.1", port, &active_url);
        if (!already_running && g_server_bind != "0.0.0.0" && g_server_bind != "127.0.0.1")
            already_running = ProbeRunningService(g_server_bind.c_str(), port, &active_url);
        for (const auto& ip : ips) {
            if (already_running) break;
            already_running = ProbeRunningService(ip.c_str(), port, &active_url);
        }

        if (already_running) {
            g_server_url = active_url;
            LogKlog("Verified active PS5 Network Mounting service at " + active_url + "; refreshing its panel");
            SendToastWithRetries("PS5 Network Mounting v" + std::string(kVersion) + "\nAlready active: " + active_url, 3, 500);
            if (std::getenv("PS5NM_NO_BROWSER") == nullptr || std::strcmp(std::getenv("PS5NM_NO_BROWSER"), "1") != 0)
                (void)LaunchBrowserWithRetries(active_url, 5, 1000);
            if (net_pool >= 0) (void)sceNetPoolDestroy(net_pool);
            return 0;
        }

        SendToastWithRetries("PS5 Network Mounting Error:\nCould not bind port " + std::to_string(port), 3, 500);
        if (net_pool >= 0) (void)sceNetPoolDestroy(net_pool);
        return 3;
    }

    std::string recovery_error;
    if (!g_mount.RecoverStaleMount(&recovery_error))
        LogKlog("Startup mount recovery failed: " + recovery_error);

    LogKlog("Listening on " + std::string(bind_ip) + ":" + std::to_string(port) + " (URL: " + web_url + ")");

    // Spawn background startup worker to dispatch toast & browser without delaying the accept loop
    auto* startup_args = new StartupWorkerArgs{web_url, "PS5 Network Mounting v" + std::string(kVersion) + "\nListening on: " + web_url};
    pthread_t startup_tid;
    if (pthread_create(&startup_tid, nullptr, StartupWorker, startup_args) == 0) {
        pthread_detach(startup_tid);
    } else {
        StartupWorker(startup_args);
    }

    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(server, &read_set);
        timeval wait{1, 0};
        const int ready = select(server + 1, &read_set, nullptr, nullptr, &wait);
        if (g_stop_requested) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            LogKlog("select on HTTP listener failed (errno " + std::to_string(errno) + ")");
            break;
        }
        if (ready == 0) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (g_stop_requested) break;
            continue;
        }

        pthread_mutex_lock(&g_client_count_lock);
        if (g_active_clients >= kMaxConcurrentClients) {
            pthread_mutex_unlock(&g_client_count_lock);
            HttpResponse busy = ApiError(503, "busy", "Too many concurrent connections.");
            SendResponse(client, busy);
            close(client);
            continue;
        }
        ++g_active_clients;
        pthread_mutex_unlock(&g_client_count_lock);

        int* arg = new int(client);
        pthread_t th;
        if (pthread_create(&th, nullptr, ClientWorker, arg) == 0) {
            pthread_detach(th);
        } else {
            // Fallback to synchronous if thread creation fails
            delete arg;
            HandleClient(client);
            close(client);
            pthread_mutex_lock(&g_client_count_lock);
            --g_active_clients;
            pthread_mutex_unlock(&g_client_count_lock);
        }
    }

    LogKlog("Shutting down HTTP listener and active network mount");
    close(server);
    g_server_socket = -1;
    pthread_mutex_lock(&g_mount_api_lock);
    std::string unmount_error;
    if (!g_mount.Unmount("/hostapp", &unmount_error) && !unmount_error.empty())
        LogKlog("Mount cleanup during shutdown failed: " + unmount_error);
    pthread_mutex_unlock(&g_mount_api_lock);
    if (net_pool >= 0) (void)sceNetPoolDestroy(net_pool);
    return 0;
}
