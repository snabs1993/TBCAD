#include "key_map.h"

#include <cctype>
#include <map>
#include <string>

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

std::optional<std::string> keyNameFor(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return std::string(1, static_cast<char>('A' + (key - ImGuiKey_A)));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return std::string(1, static_cast<char>('0' + (key - ImGuiKey_0)));
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return "F" + std::to_string(key - ImGuiKey_F1 + 1);
    // Search the name table so both directions stay in step.
    static const char* const names[] = {"ESCAPE", "ENTER",  "DELETE",   "TAB",       "SPACE",
                                        "HOME",   "END",    "LEFT",     "RIGHT",     "UP",
                                        "DOWN",   "PAGEUP", "PAGEDOWN", "BACKSPACE", "INSERT"};
    for (const char* n : names) {
        auto k = imguiKeyFor(n);
        if (k && *k == key)
            return std::string(n);
    }
    return std::nullopt;
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
