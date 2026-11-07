#pragma once

#include <span>
#include <string>
#include <string_view>

namespace tbcad::ui {

// "Keyboard layout for commands" (setting input.commandKeyboard): with the system keyboard set to
// another language, the letters typed for a command name, an option or a number are read as the
// key in the same place on an English (US) keyboard. With Thai Kedmanee, "ส" is the L key, so
// typing it starts LINE without switching the language first. Text answers stay as typed.
struct CommandKeyboard {
    const char* id;     // the setting value
    const char* label;  // shown in Settings
};

inline constexpr const char* kCommandKeyboardSetting = "input.commandKeyboard";

// "off" first, then every layout that can be chosen. New layouts are added to the table in
// command_keys.cpp.
std::span<const CommandKeyboard> commandKeyboards();

// The US-keyboard character on the same key as `ch` in the layout `id`, or `ch` itself when the
// layout does not have it (or `id` is "off" or unknown). `layoutTyping` says the system reports
// that layout as the one typing now: only then do the ASCII characters the layout puts on other
// keys move too ("/" is on the 2 key in Thai Kedmanee). Without it only the layout's own letters
// are mapped, so a "/" typed on an English keyboard stays a "/".
char32_t commandKey(std::string_view id, char32_t ch, bool layoutTyping);

// commandKey for every character of a UTF-8 string.
std::string toCommandKeys(std::string_view id, std::string_view utf8, bool layoutTyping);

}  // namespace tbcad::ui
