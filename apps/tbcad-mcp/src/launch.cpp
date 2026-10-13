// Starting tbcad from tbcad-mcp. This file may use system headers (it lives under apps/).

#include <chrono>
#include <iostream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
extern char** environ;
#endif

#include "tbcad/mcp/http_server.h"
#include "tbcad/mcpcli/cli.h"

namespace tbcad::mcpcli {

bool launchTbcad(const Environment& env, std::ostream& err) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    fs::path exe = env.self.parent_path() / "tbcad.exe";
#else
    fs::path exe = env.self.parent_path() / "tbcad";
#endif
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        err << "tbcad-mcp: cannot find " << exe.string() << "\n";
        return false;
    }
#ifdef _WIN32
    std::wstring cmdline = L"\"" + exe.wstring() + L"\" --mcp";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.wstring().c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        err << "tbcad-mcp: cannot start " << exe.string() << "\n";
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    std::string path = exe.string();
    char* argv[] = {path.data(), const_cast<char*>("--mcp"), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ) != 0) {
        err << "tbcad-mcp: cannot start " << path << "\n";
        return false;
    }
#endif
    auto file = mcp::discoveryPath(env.dataDir);
    for (int i = 0; i < 100; ++i) {  // up to 20 s
        if (mcp::readDiscovery(file))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    err << "tbcad-mcp: TBCAD did not publish its MCP address within 20 seconds\n";
    return false;
}

}  // namespace tbcad::mcpcli
