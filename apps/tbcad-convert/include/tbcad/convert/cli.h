// The tbcad-convert command line, as a function so tests can run it in-process.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tbcad::convert {

// Exit codes of tbcad-convert.
inline constexpr int kExitOk = 0;        // converted, no warnings
inline constexpr int kExitWarnings = 1;  // converted, with warnings (see the report)
inline constexpr int kExitFailed = 2;    // nothing written, or bad command line

// `args` excludes the program name. Messages go to `out` (summary) and `err` (problems).
int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace tbcad::convert
