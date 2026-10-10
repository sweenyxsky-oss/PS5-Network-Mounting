#pragma once
#include <string>
namespace ps5nm {
bool StartSmbFuse(const std::string& host, int port, const std::string& share_and_path,
                  const std::string& mountpoint, const std::string& username,
                  const std::string& password, std::string* error);
bool StopSmbFuse(const std::string& mountpoint, std::string* error);
bool SmbFuseIsMounted();
}
