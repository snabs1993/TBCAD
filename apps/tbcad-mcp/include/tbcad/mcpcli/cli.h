#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace tbcad::mcpcli {

struct Options {
    bool headless = false;
    std::optional<std::filesystem::path> file{};
    bool readOnly = false;
    bool launch = false;
    std::optional<std::filesystem::path> logFile{};
    bool printConfig = false;
    bool help = false;
    bool version = false;
    std::string error{};
};

Options parseArgs(const std::vector<std::string>& args);
std::string usage();

struct Environment {
    std::filesystem::path self{};     // path of tbcad-mcp itself
    std::filesystem::path dataDir{};  // where mcp.json lives
    std::string version = "0";
};

// Runs the program with the given streams (stdin/stdout/stderr in production). Returns the exit
// code. Headless mode needs `registerCommands`-equipped registry; see run.cpp.
int run(const Options& options, const Environment& env, std::istream& in, std::ostream& out,
        std::ostream& err);

// Text printed by --print-config.
std::string configText(const std::filesystem::path& exe);

// Starts tbcad (next to tbcad-mcp) with --mcp and waits for the discovery file. Platform code.
bool launchTbcad(const Environment& env, std::ostream& err);

}  // namespace tbcad::mcpcli
