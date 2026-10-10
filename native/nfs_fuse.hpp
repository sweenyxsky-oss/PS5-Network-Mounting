#pragma once
#include <string>

namespace ps5nm {
// Starts a read-only NFSv3/v4 FUSE mount. Returns true only after the FUSE
// channel and filesystem object have been created successfully.
bool StartNfsFuse(const std::string& host, int port, const std::string& export_path,
                  const std::string& mountpoint, std::string* error);
bool StopNfsFuse(const std::string& mountpoint, std::string* error);
bool NfsFuseIsMounted();
}
