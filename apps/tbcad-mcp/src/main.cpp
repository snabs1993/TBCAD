#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "tbcad/mcpcli/cli.h"
#include "tbcad/platform/paths.h"

namespace {

std::filesystem::path selfPath(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n > 0)
        return std::filesystem::path(std::wstring(buf, n));
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return p;
#endif
    return std::filesystem::absolute(argv0);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // The protocol is UTF-8 text with \n line ends: no translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::vector<std::string> args(argv + 1, argv + argc);
    tbcad::mcpcli::Options options = tbcad::mcpcli::parseArgs(args);
    tbcad::mcpcli::Environment env;
    env.self = selfPath(argv[0]);
    env.dataDir = tbcad::platform::defaultAppPaths().dataDir;
#ifdef TBCAD_VERSION
    env.version = TBCAD_VERSION;
#endif
    return tbcad::mcpcli::run(options, env, std::cin, std::cout, std::cerr);
}
