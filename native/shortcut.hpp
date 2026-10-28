#pragma once

#include <string>

namespace ps5nm {
struct ShortcutResult {
    bool success = false;
    bool shadow_mountplus_detected = false;
    unsigned int http_status = 500;
    std::string path;
    std::string message;
};

ShortcutResult CreateHomeScreenShortcut(const std::string& launcher_elf);
}
