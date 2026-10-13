#pragma once

#include <imgui.h>

#include <optional>

#include "tbcad/platform/keybindings.h"

namespace tbcad::app {

// ImGuiKey for a chord's key name ("Z", "F5", "ESCAPE" ...).
std::optional<ImGuiKey> imguiKeyFor(const std::string& keyName);
// ImGui key chord (modifiers | key) for IsKeyChordPressed; nullopt when the key is unknown.
std::optional<ImGuiKeyChord> imguiChordFor(const platform::KeyChord& chord);

}  // namespace tbcad::app
