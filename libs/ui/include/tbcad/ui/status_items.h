#pragma once

#include <string>
#include <string_view>
#include <vector>

// Which items the status bar shows. The status bar's customization menu (the button at its end)
// hides and shows them; the hidden ids are kept, comma separated, in one setting.
namespace tbcad::ui {

inline constexpr const char* kStatusHiddenSetting = "statusbar.hidden";

inline std::vector<std::string> statusHiddenIds(std::string_view setting) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : std::string(setting) + ',') {
        if (c == ',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur.push_back(c);
        }
    }
    return out;
}

inline bool statusItemShown(std::string_view setting, std::string_view id) {
    for (const std::string& h : statusHiddenIds(setting))
        if (h == id)
            return false;
    return true;
}

// The setting with `id` shown or hidden.
inline std::string setStatusItemShown(std::string_view setting, std::string_view id, bool shown) {
    std::string out;
    bool listed = false;
    for (const std::string& h : statusHiddenIds(setting)) {
        if (h == id) {
            listed = true;
            if (shown)
                continue;
        }
        out += (out.empty() ? "" : ",") + h;
    }
    if (!shown && !listed && !id.empty())
        out += (out.empty() ? "" : ",") + std::string(id);
    return out;
}

}  // namespace tbcad::ui
