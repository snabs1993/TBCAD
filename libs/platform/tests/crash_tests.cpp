#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/platform/crash.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace fs = std::filesystem;
using namespace tbcad::platform;

static fs::path tempDir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

TEST_CASE("crash reports are listed oldest first and can be acknowledged") {
    auto d = tempDir("tbcad_crash_list");
    std::ofstream(d / "crash-20260101-000002.txt") << "b";
    std::ofstream(d / "crash-20260101-000001.txt") << "a";
    std::ofstream(d / "crash-20260101-000001.dmp") << "x";
    std::ofstream(d / "other.txt") << "x";
    auto list = pendingCrashReports(d);
    REQUIRE(list.size() == 2);
    CHECK(list[0].filename() == "crash-20260101-000001.txt");
    acknowledgeCrashReports(d);
    CHECK(pendingCrashReports(d).empty());
    CHECK(fs::exists(d / "seen-20260101-000001.txt"));
    CHECK(fs::exists(d / "crash-20260101-000001.dmp"));
}

#ifndef _WIN32
TEST_CASE("a segfault writes a report with a stack trace") {
    auto d = tempDir("tbcad_crash_segv");
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        installCrashHandler(d, "test-1.0");
        std::raise(SIGSEGV);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));
    auto list = pendingCrashReports(d);
    REQUIRE(list.size() == 1);
    std::ifstream in(list[0]);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("TBCAD test-1.0 crashed") != std::string::npos);
    CHECK(text.find("signal 11") != std::string::npos);
}
#endif
