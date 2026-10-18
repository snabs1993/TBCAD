#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tbcad::ui {

// The lines typed on the command line, for the Up and Down keys. Like AutoCAD: Up goes back
// through what was typed, Down comes forward again and ends on what was being typed before the
// first Up. A line equal to the last one is not stored twice, and the list is capped.
class InputHistory {
public:
    explicit InputHistory(std::size_t limit = 500) : limit_(limit) {}

    // Remembers a submitted line and returns to "not browsing".
    void push(const std::string& line);
    // Up: the older line, `current` being what is in the box (kept as the draft on the first Up).
    // Returns the text to show; unchanged `current` when there is no history.
    std::string older(const std::string& current);
    // Down: the newer line, or the draft after the newest. `current` is returned when not browsing.
    std::string newer(const std::string& current);
    bool browsing() const { return pos_ >= 0; }
    void stopBrowsing() { pos_ = -1; }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_{};
    std::size_t limit_;
    int pos_ = -1;
    std::string draft_{};
};

}  // namespace tbcad::ui
