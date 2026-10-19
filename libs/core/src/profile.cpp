#include "tbcad/core/profile.h"

#include <cstdio>
#include <cstdlib>

namespace tbcad::profile {

bool editOn() {
    static const bool on = [] {
        for (const char* var : {"TBCAD_EDIT_PROFILE", "TBCAD_INDEX_PROFILE"})
            if (const char* v = std::getenv(var); v && *v && *v != '0')
                return true;
        return false;
    }();
    return on;
}

EditTimer::EditTimer(std::string what, std::size_t count, double minMs)
    : count_(count), minMs_(minMs), on_(editOn()) {
    if (on_) {
        what_ = std::move(what);
        start_ = std::chrono::steady_clock::now();
    }
}

EditTimer::~EditTimer() {
    if (!on_)
        return;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
            .count();
    if (ms < minMs_)
        return;
    if (count_)
        std::fprintf(stderr, "edit profile: %s took %.1f ms (%zu objects)\n", what_.c_str(), ms,
                     count_);
    else
        std::fprintf(stderr, "edit profile: %s took %.1f ms\n", what_.c_str(), ms);
}

}  // namespace tbcad::profile
