#include "gl_problem.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#endif

namespace tbcad {

#ifdef _WIN32
namespace {
std::filesystem::path softwareDll() {
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof *buf));
    if (n == 0)
        return {};
    return std::filesystem::path(buf).parent_path() / "software-gl" / "opengl32.dll";
}
}  // namespace
#endif

bool showOpenGlProblem(const std::string& text, bool offerSoftware) {
#ifdef _WIN32
    if (offerSoftware) {
        const std::string ask = text +
                                "\n\nStart TBCAD with software drawing instead? It works without "
                                "a graphics driver but is slower.";
        return MessageBoxA(nullptr, ask.c_str(), "TBCAD cannot start", MB_YESNO | MB_ICONERROR) ==
               IDYES;
    }
    MessageBoxA(nullptr, text.c_str(), "TBCAD cannot start", MB_OK | MB_ICONERROR);
#else
    (void)text;
    (void)offerSoftware;
#endif
    return false;
}

bool softwareGlAvailable() {
#ifdef _WIN32
    std::error_code ec;
    const auto dll = softwareDll();
    return !dll.empty() && std::filesystem::exists(dll, ec);
#else
    return false;
#endif
}

bool useSoftwareGl() {
#ifdef _WIN32
    // Loading it by full path first makes the later LoadLibrary("opengl32.dll") of GLFW reuse it.
    return softwareGlAvailable() && LoadLibraryW(softwareDll().c_str()) != nullptr;
#else
    return false;
#endif
}

bool relaunchWithSoftwareGl() {
#ifdef _WIN32
    std::wstring cmd = GetCommandLineW();
    cmd += L" --software-gl";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return false;
#endif
}

}  // namespace tbcad
