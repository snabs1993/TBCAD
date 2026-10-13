#include "key_map.h"

#include <cctype>
#include <map>

namespace tbcad::app {

std::optional<ImGuiKey> imguiKeyFor(const std::string& k) {
    if (k.size() == 1) {
        char c = k[0];
        if (c >= 'A' && c <= 'Z')
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        if (c >= '0' && c <= '9')
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        return std::nullopt;
    }
    if (k.size() >= 2 && k[0] == 'F' && std::isdigit(static_cast<unsigned char>(k[1]))) {
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24)
            return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
        return std::nullopt;
    }
    static const std::map<std::string, ImGuiKey> names = {
        {"ESCAPE", ImGuiKey_Escape},     {"ENTER", ImGuiKey_Enter},
        {"DELETE", ImGuiKey_Delete},     {"TAB", ImGuiKey_Tab},
        {"SPACE", ImGuiKey_Space},       {"HOME", ImGuiKey_Home},
        {"END", ImGuiKey_End},           {"LEFT", ImGuiKey_LeftArrow},
        {"RIGHT", ImGuiKey_RightArrow},  {"UP", ImGuiKey_UpArrow},
        {"DOWN", ImGuiKey_DownArrow},    {"PAGEUP", ImGuiKey_PageUp},
        {"PAGEDOWN", ImGuiKey_PageDown}, {"BACKSPACE", ImGuiKey_Backspace},
        {"INSERT", ImGuiKey_Insert}};
    auto it = names.find(k);
    if (it == names.end())
        return std::nullopt;
    return it->second;
}

std::optional<ImGuiKeyChord> imguiChordFor(const platform::KeyChord& chord) {
    auto key = imguiKeyFor(chord.key);
    if (!key)
        return std::nullopt;
    ImGuiKeyChord c = *key;
    if (chord.ctrl)
        c |= ImGuiMod_Ctrl;
    if (chord.shift)
        c |= ImGuiMod_Shift;
    if (chord.alt)
        c |= ImGuiMod_Alt;
    return c;
}

}  // namespace tbcad::app
