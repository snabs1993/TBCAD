#include "tbcad/ui/input_history.h"

#include <algorithm>

namespace tbcad::ui {

void InputHistory::push(const std::string& line) {
    pos_ = -1;
    draft_.clear();
    if (line.empty() || (!lines_.empty() && lines_.back() == line))
        return;
    lines_.push_back(line);
    if (lines_.size() > limit_)
        lines_.erase(lines_.begin(),
                     lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - limit_));
}

std::string InputHistory::older(const std::string& current) {
    if (lines_.empty())
        return current;
    if (pos_ < 0) {
        draft_ = current;
        pos_ = static_cast<int>(lines_.size()) - 1;
    } else {
        pos_ = std::max(0, pos_ - 1);
    }
    return lines_[static_cast<std::size_t>(pos_)];
}

std::string InputHistory::newer(const std::string& current) {
    if (pos_ < 0)
        return current;
    ++pos_;
    if (pos_ >= static_cast<int>(lines_.size())) {
        pos_ = -1;
        return draft_;
    }
    return lines_[static_cast<std::size_t>(pos_)];
}

}  // namespace tbcad::ui
