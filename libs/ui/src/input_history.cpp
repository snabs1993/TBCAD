#include "tbcad/ui/input_history.h"

#include <algorithm>
#include <cctype>

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

namespace {

bool startsWith(const std::string& line, const std::string& prefix) {
    if (prefix.size() > line.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

}  // namespace

std::string InputHistory::older(const std::string& current) {
    int from = pos_ < 0 ? static_cast<int>(lines_.size()) : pos_;
    const std::string& prefix = pos_ < 0 ? current : draft_;
    for (int i = from - 1; i >= 0; --i)
        if (startsWith(lines_[static_cast<std::size_t>(i)], prefix)) {
            if (pos_ < 0)
                draft_ = current;
            pos_ = i;
            return lines_[static_cast<std::size_t>(i)];
        }
    // Nothing older that matches: stay on the line shown.
    return pos_ < 0 ? current : lines_[static_cast<std::size_t>(pos_)];
}

std::string InputHistory::newer(const std::string& current) {
    if (pos_ < 0)
        return current;
    for (int i = pos_ + 1; i < static_cast<int>(lines_.size()); ++i)
        if (startsWith(lines_[static_cast<std::size_t>(i)], draft_)) {
            pos_ = i;
            return lines_[static_cast<std::size_t>(i)];
        }
    pos_ = -1;
    return draft_;
}

}  // namespace tbcad::ui
