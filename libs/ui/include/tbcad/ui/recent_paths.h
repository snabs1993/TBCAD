#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// A short history of drawing files kept in one setting (the content browser and the Other Drawing
// tab of the block palette share "content.history"): paths separated by \x1e, newest first.
namespace tbcad::ui {

inline constexpr const char* kDrawingHistorySetting = "content.history";
inline constexpr std::size_t kMaxDrawingHistory = 12;

inline std::vector<std::string> parsePathHistory(const std::string& setting) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : setting + '\x1e') {
        if (c == '\x1e') {
            if (!cur.empty() && out.size() < kMaxDrawingHistory)
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

// Puts `path` first (an older copy of it removed; case-insensitive, / and \ the same).
inline std::string pushPathHistory(const std::string& setting, const std::string& path) {
    auto key = [](std::string s) {
        for (char& c : s)
            c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::vector<std::string> list = parsePathHistory(setting);
    std::erase_if(list, [&](const std::string& p) { return key(p) == key(path); });
    if (!path.empty())
        list.insert(list.begin(), path);
    if (list.size() > kMaxDrawingHistory)
        list.resize(kMaxDrawingHistory);
    std::string out;
    for (const std::string& p : list)
        out += (out.empty() ? "" : "\x1e") + p;
    return out;
}

}  // namespace tbcad::ui
