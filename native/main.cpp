#include <microhttpd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <unistd.h>
#include <string>

#include "mount_backend.hpp"
#include "web_assets.hpp"

namespace {
constexpr unsigned short kDefaultPort = 18080;
constexpr size_t kMaxBody = 16 * 1024;
ps5nm::MountBackend g_mount;

struct RequestState { std::string body; bool replied = false; };

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

// Small, bounded JSON string reader for this fixed API. It rejects escaped
// control sequences instead of trying to interpret ambiguous path data.
bool JsonString(const std::string& body, const char* key, std::string* out) {
    const std::string needle = std::string("\"") + key + "\"";
    auto p = body.find(needle);
    if (p == std::string::npos) return false;
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
    if (p >= body.size() || body[p++] != '"') return false;
    std::string value;
    while (p < body.size()) {
        char c = body[p++];
        if (c == '"') { *out = value; return true; }
        if (c == '\\') {
            if (p >= body.size()) return false;
            char e = body[p++];
            if (e == '"' || e == '\\' || e == '/') value += e;
            else return false;
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            value += c;
        }
    }
    return false;
}
bool JsonInt(const std::string& body, const char* key, int* out) {
    const std::string needle = std::string("\"") + key + "\"";
    auto p = body.find(needle);
    if (p == std::string::npos) return false;
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
    char* end = nullptr;
    long v = std::strtol(body.c_str() + p, &end, 10);
    if (end == body.c_str() + p || v < 1 || v > 65535) return false;
    *out = static_cast<int>(v);
    return true;
}
MHD_Result Reply(MHD_Connection* conn, unsigned int status, const std::string& body,
                 const char* content_type = "application/json; charset=utf-8") {
    MHD_Response* response = MHD_create_response_from_buffer(body.size(),
        const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    MHD_add_response_header(response, "Content-Type", content_type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Connection", "close");
    MHD_Result result = MHD_queue_response(conn, status, response);
    MHD_destroy_response(response);
    return result;
}
MHD_Result Status(MHD_Connection* conn) {
    const auto s = g_mount.Status();
    std::string body = "{\"state\":\"ready\",\"mounted\":" +
        std::string(s.mounted ? "true" : "false") + ",\"protocol\":" +
        (s.protocol.empty() ? "null" : Quote(s.protocol)) + ",\"endpoint\":" +
        (s.endpoint.empty() ? "null" : Quote(s.endpoint)) + ",\"mountpoint\":" +
        Quote(s.mountpoint) + ",\"last_result\":" + Quote(s.last_result) +
        ",\"last_error\":" + (s.last_error.empty() ? "null" : Quote(s.last_error)) + "}";
    return Reply(conn, MHD_HTTP_OK, body);
}
MHD_Result ApiError(MHD_Connection* conn, unsigned int status, const char* code,
                    const std::string& message) {
    return Reply(conn, status, "{\"ok\":false,\"error\":" + Quote(code) +
                 ",\"message\":" + Quote(message) + "}");
}
void Completed(void*, MHD_Connection*, void** con_cls, MHD_RequestTerminationCode) {
    delete static_cast<RequestState*>(*con_cls);
    *con_cls = nullptr;
}
MHD_Result Access(void*, MHD_Connection* conn, const char* url, const char* method,
                  const char*, const char* upload_data, size_t* upload_data_size, void** con_cls) {
    if (!*con_cls) {
        *con_cls = new (std::nothrow) RequestState{};
        return *con_cls ? MHD_YES : MHD_NO;
    }
    auto* state = static_cast<RequestState*>(*con_cls);
    if (*upload_data_size) {
        if (state->body.size() + *upload_data_size > kMaxBody) {
            *upload_data_size = 0;
            state->replied = true;
            return ApiError(conn, MHD_HTTP_CONTENT_TOO_LARGE, "body_too_large",
                            "Request body exceeds the 16 KiB limit.");
        }
        state->body.append(upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }
    if (state->replied) return MHD_YES;
    state->replied = true;
    const std::string path(url ? url : "/");
    const std::string verb(method ? method : "GET");
    if (verb == "GET" && (path == "/" || path == "/index.html"))
        return Reply(conn, MHD_HTTP_OK, std::string(ps5nm::kIndexHtml), "text/html; charset=utf-8");
    if (verb == "GET" && path == "/api/v1/status") return Status(conn);
    if (verb == "POST" && path == "/api/v1/mount") {
        std::string protocol, host, remote, mountpoint, username, password;
        int port = 2049;
        if (!JsonString(state->body, "protocol", &protocol) ||
            !JsonString(state->body, "host", &host) ||
            !JsonString(state->body, "remote_path", &remote) ||
            !JsonString(state->body, "mountpoint", &mountpoint))
            return ApiError(conn, MHD_HTTP_BAD_REQUEST, "invalid_request",
                            "Required fields are missing or malformed: protocol, host, remote_path, mountpoint.");
        (void)JsonString(state->body, "username", &username);
        (void)JsonString(state->body, "password", &password);
        (void)username; (void)password; // Never log or echo credentials.
        (void)JsonInt(state->body, "port", &port);
        if (host.empty() || host.size() > 253 ||
            host.find_first_of(" /\\\r\n\t") != std::string::npos)
            return ApiError(conn, MHD_HTTP_BAD_REQUEST, "invalid_host", "Enter a hostname or IP address only.");
        if (remote.empty() || remote.front() != '/' || remote.find("..") != std::string::npos ||
            remote.size() > 1024)
            return ApiError(conn, MHD_HTTP_BAD_REQUEST, "invalid_remote_path",
                            "Remote export path must be an absolute path without '..'.");
        if (mountpoint != "/hostapp")
            return ApiError(conn, MHD_HTTP_BAD_REQUEST, "unsafe_mountpoint",
                            "For safety, the only allowed mountpoint is /hostapp.");
        std::string error;
        if (!g_mount.Mount(protocol, host, port, remote, mountpoint, &error))
            return ApiError(conn, MHD_HTTP_NOT_IMPLEMENTED, "mount_adapter_unavailable", error);
        return Status(conn);
    }
    if (verb == "POST" && path == "/api/v1/unmount") {
        std::string mountpoint;
        if (!JsonString(state->body, "mountpoint", &mountpoint))
            return ApiError(conn, MHD_HTTP_BAD_REQUEST, "invalid_request", "mountpoint is required.");
        std::string error;
        if (!g_mount.Unmount(mountpoint, &error))
            return ApiError(conn, MHD_HTTP_CONFLICT, "unmount_failed", error);
        return Status(conn);
    }
    if (path.rfind("/api/", 0) == 0)
        return ApiError(conn, MHD_HTTP_NOT_FOUND, "not_found", "Unknown API endpoint.");
    return ApiError(conn, MHD_HTTP_NOT_FOUND, "not_found", "Page not found.");
}
}

int main() {
    unsigned int port = kDefaultPort;
    if (const char* env = std::getenv("PS5NM_PORT")) {
        char* end = nullptr;
        long value = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && value >= 1024 && value <= 65535)
            port = static_cast<unsigned int>(value);
    }
    // Bind only to the console loopback interface; the UI is opened locally.
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(static_cast<uint16_t>(port));
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
        static_cast<uint16_t>(port), nullptr, nullptr, &Access, nullptr,
        MHD_OPTION_SOCK_ADDR, static_cast<const sockaddr*>(static_cast<void*>(&bind_addr)),
        MHD_OPTION_NOTIFY_COMPLETED, &Completed, nullptr,
        MHD_OPTION_CONNECTION_TIMEOUT, static_cast<unsigned int>(15),
        MHD_OPTION_THREAD_POOL_SIZE, static_cast<unsigned int>(4),
        MHD_OPTION_END);
    if (!daemon) return 2;
    // A payload normally stays resident; a graceful process termination is
    // handled by the OS. The SDK's process lifecycle ends the HTTP daemon.
    for (;;) sleep(3600);
    MHD_stop_daemon(daemon);
    return 0;
}
