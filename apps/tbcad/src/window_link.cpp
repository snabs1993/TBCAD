#include "window_link.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tbcad/platform/paths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tbcad::app {

namespace fs = std::filesystem;

namespace {

constexpr double kAliveSeconds = 6.0;   // a window not heard from for this long is gone
constexpr double kStaleSeconds = 60.0;  // its files are then removed

long currentPid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Seconds since the file was last written; a large number when that cannot be told.
double ageSeconds(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec)
        return 1e9;
    return std::chrono::duration<double>(fs::file_time_type::clock::now() - t).count();
}

// Writes next to the target and renames, so a reader never sees half a file.
bool writeAtomic(const fs::path& target, const std::string& text) {
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

long WindowLink::currentProcessId() {
    return currentPid();
}

WindowLink::WindowLink(fs::path dir) : WindowLink(std::move(dir), currentPid()) {}

WindowLink::WindowLink(fs::path dir, long pid) : dir_(std::move(dir)), pid_(pid) {
    platform::ensureDirectory(dir_);
}

WindowLink::~WindowLink() {
    std::error_code ec;
    fs::remove(winFile(pid_), ec);
}

fs::path WindowLink::winFile(long pid) const {
    return dir_ / (std::to_string(pid) + ".win");
}

void WindowLink::publish(const WindowRect& r, bool force) {
    const double now = nowSeconds();
    if (!force && now - lastPublish_ < 0.4)
        return;
    lastPublish_ = now;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%.1f %.1f %.1f %.1f\n", static_cast<double>(r.x),
                  static_cast<double>(r.y), static_cast<double>(r.w), static_cast<double>(r.h));
    (void)writeAtomic(winFile(pid_), buf);
}

std::vector<OtherWindow> WindowLink::others() {
    std::vector<OtherWindow> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (!endsWith(name, ".win"))
            continue;
        const long pid = std::strtol(name.c_str(), nullptr, 10);
        if (pid <= 0 || pid == pid_)
            continue;
        const double age = ageSeconds(p);
        if (age > kStaleSeconds) {
            std::error_code rm;
            fs::remove(p, rm);
            continue;
        }
        if (age > kAliveSeconds)
            continue;
        std::istringstream in(readAll(p));
        OtherWindow w;
        w.pid = pid;
        if (in >> w.rect.x >> w.rect.y >> w.rect.w >> w.rect.h)
            out.push_back(w);
    }
    return out;
}

bool WindowLink::send(long pid, const fs::path& file) {
    const std::string name = std::to_string(pid) + ".open." + std::to_string(pid_) + "-" +
                             std::to_string(counter_++) + "-" +
                             std::to_string(static_cast<long long>(nowSeconds() * 1000));
    return writeAtomic(dir_ / name, platform::pathToUtf8(file));
}

std::vector<std::string> WindowLink::takeIncoming() {
    std::vector<std::string> out;
    const std::string prefix = std::to_string(pid_) + ".open.";
    std::error_code ec;
    std::vector<fs::path> mine;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0 && !endsWith(name, ".tmp"))
            mine.push_back(it->path());
    }
    for (const fs::path& p : mine) {
        std::string text = readAll(p);
        std::error_code rm;
        fs::remove(p, rm);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty())
            out.push_back(std::move(text));
    }
    return out;
}

bool WindowLink::spawn(const std::string& file, int x, int y, int w, int h) {
    const std::string pos = std::to_string(x) + "," + std::to_string(y);
    const std::string size = std::to_string(w) + "," + std::to_string(h);
#ifdef _WIN32
    wchar_t exe[4096];
    const DWORD n = GetModuleFileNameW(nullptr, exe, 4096);
    if (n == 0 || n >= 4096)
        return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child --window-pos " +
                       std::wstring(pos.begin(), pos.end()) + L" --window-size " +
                       std::wstring(size.begin(), size.end()) + L" \"" +
                       platform::pathFromUtf8(file).wstring() + L"\"";
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
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return false;
    const std::string exeText = exe.string();
    const pid_t first = fork();
    if (first < 0)
        return false;
    if (first == 0) {
        // Twice, so the new window is nobody's child and leaves no zombie behind.
        if (fork() == 0) {
            execl(exeText.c_str(), exeText.c_str(), "--child", "--window-pos", pos.c_str(),
                  "--window-size", size.c_str(), file.c_str(), static_cast<char*>(nullptr));
        }
        _exit(0);
    }
    int status = 0;
    waitpid(first, &status, 0);
    return true;
#endif
}

}  // namespace tbcad::app
