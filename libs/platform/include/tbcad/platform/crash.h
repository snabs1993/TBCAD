#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tbcad::platform {

// Crash reports. After installCrashHandler, a fatal signal or unhandled exception writes
// `crash-<time>.txt` (what happened and a stack trace) into `dir`; on Windows it also writes a
// `crash-<time>.dmp` minidump. Nothing is sent anywhere: the files stay on this PC until the user
// attaches them to a bug report.
//   `appVersion` goes into the report header. Call once, early in main.
void installCrashHandler(const std::filesystem::path& dir, const std::string& appVersion);

// Crash reports in `dir` (the .txt files), oldest first, that were not yet acknowledged.
std::vector<std::filesystem::path> pendingCrashReports(const std::filesystem::path& dir);

// Marks every report in `dir` as seen so pendingCrashReports no longer lists them (renames
// crash-*.txt to seen-*.txt; the .dmp files are left next to them).
void acknowledgeCrashReports(const std::filesystem::path& dir);

}  // namespace tbcad::platform
