#include <doctest/doctest.h>

#include "tbcad/ui/theme.h"

using namespace tbcad;
using namespace tbcad::ui;

TEST_CASE("theme tokens match the UX plan") {
    CHECK(darkTheme().accent == Rgba{0x3D, 0x8B, 0xFD, 255});
    CHECK(lightTheme().bgPanel == Rgba{255, 255, 255, 255});
    CHECK(themeFor(ThemeMode::System, true).bgApp == darkTheme().bgApp);
    CHECK(themeFor(ThemeMode::System, false).bgApp == lightTheme().bgApp);
    CHECK(themeFor(ThemeMode::Dark, false).bgApp == darkTheme().bgApp);
}

TEST_CASE("theme mode names round trip, unknown means dark") {
    for (ThemeMode m : {ThemeMode::Dark, ThemeMode::Light, ThemeMode::System})
        CHECK(parseThemeMode(themeModeName(m)) == m);
    CHECK(parseThemeMode("nonsense") == ThemeMode::Dark);
}

TEST_CASE("hex colours") {
    CHECK(parseHexColor("#102030") == Rgba{0x10, 0x20, 0x30, 255});
    CHECK(parseHexColor("ff000080") == Rgba{255, 0, 0, 0x80});
    CHECK_FALSE(parseHexColor("#12345").has_value());
    CHECK_FALSE(parseHexColor("#12345G").has_value());
    CHECK(toHexColor(Rgba{1, 2, 255, 255}) == "#0102FF");
    CHECK(toHexColor(Rgba{1, 2, 255, 16}) == "#0102FF10");
}

TEST_CASE("mixing and luminance") {
    CHECK(mix(Rgba{0, 0, 0, 255}, Rgba{200, 100, 50, 255}, 0.5) == Rgba{100, 50, 25, 255});
    CHECK(withAlpha(Rgba{1, 2, 3, 255}, 0.0).a == 0);
    CHECK(luminance(Rgba{255, 255, 255, 255}) > 0.99);
    CHECK(luminance(darkTheme().bgViewport) < 0.2);
    CHECK(luminance(lightTheme().bgViewport) > 0.8);
}
