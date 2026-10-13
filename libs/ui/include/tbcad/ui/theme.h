#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tbcad/core/color.h"

namespace tbcad::ui {

// Every colour the program's own UI uses comes from one of these tokens (docs/ux-ui.md §8).
// Panels and widgets never hard-code colours.
struct Theme {
    Rgba bgApp, bgPanel, bgViewport;
    Rgba border;
    Rgba text, textMuted;
    Rgba accent;    // buttons, focus, tabs, window-selection fill
    Rgba select;    // selected entities, window box outline
    Rgba crossing;  // crossing box outline and fill
    Rgba snap;      // snap markers, warnings
    Rgba danger;    // errors
    Rgba gridMinor, gridMajor;
};

enum class ThemeMode { Dark, Light, System };

Theme darkTheme();
Theme lightTheme();
// `systemDark` is only consulted for ThemeMode::System.
Theme themeFor(ThemeMode mode, bool systemDark);

// "dark" | "light" | "system"; anything else gives Dark.
ThemeMode parseThemeMode(std::string_view text);
std::string_view themeModeName(ThemeMode mode);

// "#RRGGBB" or "#RRGGBBAA" (the # is optional).
std::optional<Rgba> parseHexColor(std::string_view text);
std::string toHexColor(Rgba color);

Rgba withAlpha(Rgba color, double alpha);
// 0 gives `a`, 1 gives `b`; alpha is mixed too.
Rgba mix(Rgba a, Rgba b, double t);
// Perceived brightness 0..1 (Rec. 709).
double luminance(Rgba color);

}  // namespace tbcad::ui
