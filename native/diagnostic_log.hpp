#pragma once
#include <cstddef>
#include <string>

namespace ps5nm {
// Initialize a rotating, persistent diagnostic log. On PS5 the default is /data/PS5NetworkMounting/ps5nm.log.
bool InitDiagnosticLog();
void LogEvent(const char* level, const char* component, const std::string& message);
// Returns up to max_bytes of the current persistent log (or session buffer if storage is unavailable).
std::string ReadDiagnosticLog(size_t max_bytes, bool* persistent = nullptr);
std::string DiagnosticLogPath();
} // namespace ps5nm
