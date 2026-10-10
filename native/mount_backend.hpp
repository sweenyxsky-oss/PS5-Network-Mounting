#pragma once
#include <string>
namespace ps5nm {
struct MountStatus {
    bool mounted = false;
    bool can_disconnect = false;
    std::string protocol, endpoint;
    std::string mountpoint = "/hostapp";
    std::string last_result = "No operation yet";
    std::string last_error;
};
class MountBackend {
public:
    MountStatus Status() const;
    bool Mount(const std::string& protocol, const std::string& host, int port,
               const std::string& remote_path, const std::string& mountpoint,
               const std::string& username, const std::string& password,
               std::string* error);
    bool Unmount(const std::string& mountpoint, std::string* error);
private:
    MountStatus status_;
};
}
