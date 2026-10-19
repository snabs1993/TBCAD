#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace tbcad::profile {

/// TBCAD_EDIT_PROFILE=1 (or TBCAD_INDEX_PROFILE=1): where the time of an edit goes, on stderr.
bool editOn();

/// Prints "edit profile: <what> took N ms (<count> objects)" when it lived longer than minMs
/// and the edit profile is on.
class EditTimer {
public:
    explicit EditTimer(std::string what, std::size_t count = 0, double minMs = 5);
    ~EditTimer();
    EditTimer(const EditTimer&) = delete;
    EditTimer& operator=(const EditTimer&) = delete;

private:
    std::string what_;
    std::size_t count_ = 0;
    double minMs_ = 5;
    bool on_ = false;
    std::chrono::steady_clock::time_point start_{};
};

}  // namespace tbcad::profile
