// The tbcad-convert command line, as a function so tests can run it in-process.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tbcad::convert {

// Exit codes of tbcad-convert.
inline constexpr int kExitOk = 0;        // converted, no warnings
inline constexpr int kExitWarnings = 1;  // converted, with warnings (see the report)
inline constexpr int kExitFailed = 2;    // nothing written, or bad command line

// Runs `onExpire` on its own thread if it is still alive after `limit` (0 = never). The
// destructor stops it. tbcad-convert uses it to give up on a pathological file instead of
// running for ever.
class Watchdog {
public:
    Watchdog(std::chrono::seconds limit, std::function<void()> onExpire);
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex m_{};
    std::condition_variable cv_{};
    bool done_ = false;
    std::thread thread_{};
};

// `args` excludes the program name. Messages go to `out` (summary) and `err` (problems).
int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace tbcad::convert
