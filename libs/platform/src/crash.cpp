#include "tbcad/platform/crash.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace tbcad::platform {

namespace {

// Everything the handler touches is preallocated: allocating inside a crashed process can hang.
char g_dir[1024] = {};
char g_version[64] = {};
bool g_installed = false;

constexpr int kRingLines = 200;
constexpr int kRingWidth = 256;
char g_ring[kRingLines][kRingWidth] = {};
unsigned g_ringNext = 0;  // total lines recorded; slot = g_ringNext % kRingLines

// Writes the ring, oldest first, using only stdio on a preallocated buffer.
void dumpRing(FILE* f) {
    std::fprintf(f, "Last command lines:\n");
    unsigned total = g_ringNext;
    unsigned count = total < kRingLines ? total : kRingLines;
    for (unsigned i = total - count; i < total; ++i)
        std::fprintf(f, "  %s\n", g_ring[i % kRingLines]);
}

void makeStamp(char* out, size_t n) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::snprintf(out, n, "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

FILE* openForWrite(const char* path) {
#ifdef _WIN32
    FILE* f = nullptr;
    return fopen_s(&f, path, "w") == 0 ? f : nullptr;
#else
    return std::fopen(path, "w");
#endif
}

#ifdef _WIN32

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    char stamp[32];
    makeStamp(stamp, sizeof stamp);
    char path[1300];

    std::snprintf(path, sizeof path, "%s\\crash-%s.txt", g_dir, stamp);
    if (FILE* f = openForWrite(path)) {
        std::fprintf(f, "TBCAD %s crashed\nexception code 0x%08lx at %p\n", g_version,
                     static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                     info->ExceptionRecord->ExceptionAddress);
        void* frames[48];
        USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i)
            std::fprintf(f, "  #%u %p\n", static_cast<unsigned>(i), frames[i]);
        dumpRing(f);
        std::fclose(f);
    }

    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        auto write = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbg, "MiniDumpWriteDump")));
        std::snprintf(path, sizeof path, "%s\\crash-%s.dmp", g_dir, stamp);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && file != INVALID_HANDLE_VALUE) {
            struct ExceptionInfo {
                DWORD thread;
                EXCEPTION_POINTERS* pointers;
                BOOL clientPointers;
            } ex{GetCurrentThreadId(), info, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), file, /*MiniDumpNormal*/ 0, &ex,
                  nullptr, nullptr);
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#else

void writeAll(int fd, const char* s) {
    size_t len = std::strlen(s);
    while (len > 0) {
        ssize_t w = ::write(fd, s, len);
        if (w <= 0)
            return;
        s += w;
        len -= static_cast<size_t>(w);
    }
}

void onSignal(int sig) {
    char stamp[32];
    makeStamp(stamp, sizeof stamp);
    char path[1300];
    std::snprintf(path, sizeof path, "%s/crash-%s.txt", g_dir, stamp);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        writeAll(fd, "TBCAD ");
        writeAll(fd, g_version);
        writeAll(fd, " crashed\nsignal ");
        char num[16];
        std::snprintf(num, sizeof num, "%d", sig);
        writeAll(fd, num);
        writeAll(fd, "\n");
        void* frames[48];
        int n = ::backtrace(frames, 48);
        ::backtrace_symbols_fd(frames, n, fd);
        writeAll(fd, "Last command lines:\n");
        unsigned total = g_ringNext;
        unsigned count = total < kRingLines ? total : kRingLines;
        for (unsigned i = total - count; i < total; ++i) {
            writeAll(fd, "  ");
            writeAll(fd, g_ring[i % kRingLines]);
            writeAll(fd, "\n");
        }
        ::close(fd);
    }
    // Restore the default action and re-raise so the system still sees the crash.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

#endif

void onTerminate() {
    char stamp[32];
    makeStamp(stamp, sizeof stamp);
    char path[1300];
#ifdef _WIN32
    std::snprintf(path, sizeof path, "%s\\crash-%s.txt", g_dir, stamp);
#else
    std::snprintf(path, sizeof path, "%s/crash-%s.txt", g_dir, stamp);
#endif
    if (FILE* f = openForWrite(path)) {
        std::fprintf(f, "TBCAD %s crashed\nuncaught C++ exception (std::terminate)\n", g_version);
        dumpRing(f);
        std::fclose(f);
    }
    std::abort();
}

}  // namespace

void installCrashHandler(const std::filesystem::path& dir, const std::string& appVersion) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string d = dir.string();
    std::snprintf(g_dir, sizeof g_dir, "%s", d.c_str());
    std::snprintf(g_version, sizeof g_version, "%s", appVersion.c_str());
    g_installed = true;
    std::set_terminate(onTerminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        ::signal(sig, onSignal);
#endif
}

void recordCommandLine(const std::string& line) {
    char* slot = g_ring[g_ringNext % kRingLines];
    size_t n = 0;
    for (char c : line) {
        if (n + 1 >= kRingWidth)
            break;
        slot[n++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    slot[n] = '\0';
    ++g_ringNext;
}

std::vector<std::filesystem::path> pendingCrashReports(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        auto name = e.path().filename().string();
        if (name.rfind("crash-", 0) == 0 && e.path().extension() == ".txt")
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void acknowledgeCrashReports(const std::filesystem::path& dir) {
    for (const auto& p : pendingCrashReports(dir)) {
        std::error_code ec;
        auto name = p.filename().string();
        std::filesystem::rename(p, p.parent_path() / ("seen-" + name.substr(6)), ec);
    }
}

}  // namespace tbcad::platform
