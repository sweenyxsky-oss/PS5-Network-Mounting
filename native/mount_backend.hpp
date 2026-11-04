#pragma once

#include <string>

#include "vfs.hpp"

namespace ps5nm {

class MountBackend {
public:
    MountStatus Status() const;
    bool Mount(const std::string& protocol, const std::string& host, int port,
               const std::string& remote_path, const std::string& mountpoint,
               const std::string& username, const std::string& password,
               std::string* error);
    bool Unmount(const std::string& mountpoint, std::string* error);

private:
    std::string last_result_ = "No operation yet";
    std::string last_error_;
};

}  // namespace ps5nm
