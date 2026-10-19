// Time and memory per phase of a conversion (tbcad-convert --timing), for finding the slow or
// greedy step on a big file.
#pragma once

#include <chrono>
#include <cstddef>
#include <ostream>
#include <string_view>

namespace tbcad::convert {

struct Memory {
    std::size_t currentMb = 0;
    std::size_t peakMb = 0;
};

// The resident memory of this process now and at its highest so far.
Memory processMemory();

// Prints one line per finished phase when enabled, nothing otherwise.
class PhaseTimer {
public:
    PhaseTimer(std::ostream& err, bool enabled) : err_(err), enabled_(enabled) {}

    void phase(std::string_view name) {
        if (!enabled_)
            return;
        const auto now = Clock::now();
        const Memory m = processMemory();
        err_ << "timing: " << name << " "
             << std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count()
             << " ms, memory " << m.currentMb << " MB (peak " << m.peakMb << " MB)\n";
        last_ = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    std::ostream& err_;
    bool enabled_;
    Clock::time_point last_ = Clock::now();
};

}  // namespace tbcad::convert
