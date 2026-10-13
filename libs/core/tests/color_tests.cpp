#include <doctest/doctest.h>

#include <set>

#include "tbcad/core/color.h"
#include "tbcad/core/entities.h"

using namespace tbcad;

TEST_CASE("Color kinds") {
    CHECK(Color{}.kind == ColorKind::ByLayer);
    CHECK(Color::byBlock().kind == ColorKind::ByBlock);
    CHECK(Color::index(5).indexValue() == 5);
    Color c = Color::rgb(1, 2, 3);
    CHECK(c.kind == ColorKind::Rgb);
    CHECK((c.r == 1 && c.g == 2 && c.b == 3));
    CHECK(Color::index(3) == Color::index(3));
    CHECK_FALSE(Color::index(3) == Color::index(4));
}

TEST_CASE("LineWeight values") {
    CHECK(LineWeight{}.isByLayer());
    CHECK(LineWeight::byBlock().isByBlock());
    CHECK(LineWeight::defaultWeight().isDefault());
    CHECK_FALSE(LineWeight::byLayer().isValue());
    CHECK(LineWeight::mm(0.25).v == 25);
    CHECK(LineWeight::mm(0.25).toMm() == doctest::Approx(0.25));
    CHECK(LineWeight::mm(-1).v == 0);
    CHECK(LineWeight::mm(1e9).v == 32767);
    CHECK(LineWeight::byBlock().toMm() == 0);
}

TEST_CASE("palette basics and distinct hues") {
    CHECK(paletteColor(1) == Rgba{255, 0, 0, 255});
    CHECK(paletteColor(2) == Rgba{255, 255, 0, 255});
    CHECK(paletteColor(3) == Rgba{0, 255, 0, 255});
    CHECK(paletteColor(4) == Rgba{0, 255, 255, 255});
    CHECK(paletteColor(5) == Rgba{0, 0, 255, 255});
    CHECK(paletteColor(6) == Rgba{255, 0, 255, 255});
    CHECK(paletteColor(7) == Rgba{255, 255, 255, 255});
    std::set<std::uint32_t> seen;
    for (int i = 1; i <= 255; ++i) {
        Rgba c = paletteColor(static_cast<std::uint8_t>(i));
        CHECK(c.a == 255);
        seen.insert((std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b);
    }
    CHECK(seen.size() > 200);  // the generated part is not degenerate
}

TEST_CASE("Entity stays small and kinds match the variant order") {
    static_assert(sizeof(Entity) <= 96);
    Entity e;
    CHECK(kind(e) == EntityKind::Line);
    e.data = CircleData{};
    CHECK(kind(e) == EntityKind::Circle);
    e.data = TextData{};
    CHECK(kind(e) == EntityKind::Text);
    e.data = ProxyData{};
    CHECK(kind(e) == EntityKind::Proxy);
    e.data = EllipseData{};
    CHECK(kind(e) == EntityKind::Ellipse);
    e.data = HatchData{};
    CHECK(kind(e) == EntityKind::Hatch);
    e.data = InsertData{};
    CHECK(kind(e) == EntityKind::Insert);
    e.data = DimensionData{};
    CHECK(kind(e) == EntityKind::Dimension);
    e.data = LeaderData{};
    CHECK(kind(e) == EntityKind::Leader);
    e.data = MTextData{};
    CHECK(kind(e) == EntityKind::MText);
    e.data = SplineData{};
    CHECK(kind(e) == EntityKind::Spline);
    e.data = PointData{};
    CHECK(kind(e) == EntityKind::Point);
    e.data = PolylineData{};
    CHECK(kind(e) == EntityKind::Polyline);
    e.data = ArcData{};
    CHECK(kind(e) == EntityKind::Arc);
}

TEST_CASE("entityAs and visitEntity unwrap boxed payloads") {
    Entity e{{}, TextData{{1, 2}, {}, 3.0, 0, 1, 0, {}, HAlign::Left, VAlign::Baseline, "hi"}};
    REQUIRE(entityAs<TextData>(e));
    CHECK(entityAs<TextData>(e)->utf8 == "hi");
    CHECK(entityAs<LineData>(e) == nullptr);
    const Entity copy = e;  // deep copy
    entityAs<TextData>(e)->utf8 = "changed";
    CHECK(entityAs<TextData>(copy)->utf8 == "hi");
    int visited = visitEntity(copy, [](const auto& d) {
        if constexpr (std::is_same_v<std::decay_t<decltype(d)>, TextData>)
            return 1;
        else
            return 0;
    });
    CHECK(visited == 1);

    Entity line{{}, LineData{{0, 0}, {1, 1}}};
    CHECK(entityAs<LineData>(line)->b == Vec2{1, 1});
}
