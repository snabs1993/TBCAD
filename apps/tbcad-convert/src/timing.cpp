#include "timing.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// (after windows.h)
#include <psapi.h>
#else
#include <fstream>
#include <string>
#endif

namespace tbcad::convert {

Memory processMemory() {
    Memory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        m.currentMb = pmc.WorkingSetSize >> 20;
        m.peakMb = pmc.PeakWorkingSetSize >> 20;
    }
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0)
            m.currentMb = std::stoul(line.substr(6)) >> 10;
        else if (line.rfind("VmHWM:", 0) == 0)
            m.peakMb = std::stoul(line.substr(6)) >> 10;
    }
#endif
    return m;
}

}  // namespace tbcad::convert
