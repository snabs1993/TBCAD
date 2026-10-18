#include <doctest/doctest.h>

#include "tbcad/gdt/gdt.h"

using namespace tbcad;

TEST_CASE("the DXF text of a frame reads back to the same frame") {
    gdt::Spec spec;
    spec.symbol = gdt::Symbol::Perpendicularity;
    spec.value = "0.05";
    spec.diameterZone = true;
    spec.material = 'M';
    spec.datums = {"A", "B"};
    const std::string text = gdt::dxfText(spec);
    CHECK(text.starts_with("{\\Fgdt;b}%%v{\\Fgdt;n}0.05{\\Fgdt;m}%%v"));
    auto back = gdt::parseDxfText(text);
    REQUIRE(back);
    CHECK(*back == spec);
    for (int i = 0; i < gdt::kSymbolCount; ++i) {
        gdt::Spec s;
        s.symbol = static_cast<gdt::Symbol>(i);
        s.value = "1";
        auto r = gdt::parseDxfText(gdt::dxfText(s));
        REQUIRE(r);
        CHECK(r->symbol == s.symbol);
    }
}

TEST_CASE("frames written by other programs are understood") {
    // Position, diameter 0.5 at maximum material, datums A, B at maximum material, C.
    auto a = gdt::parseDxfText("{\\Fgdt;j}%%v%%c0.5{\\Fgdt;m}%%v%%vA{\\Fgdt;m}%%vB%%vC%%v%%v");
    REQUIRE(a);
    CHECK(a->symbol == gdt::Symbol::Position);
    CHECK(a->diameterZone);
    CHECK(a->material == 'M');
    CHECK(a->value == "0.5");
    CHECK(a->datums == std::vector<std::string>{"A", "B", "C"});
    // A flatness with no datums and only the first line of a two-line frame.
    auto b = gdt::parseDxfText("{\\Fgdt;c}%%v0.02%%v%%v%%v%%v%%v^J{\\Fgdt;j}%%v1%%v");
    REQUIRE(b);
    CHECK(b->symbol == gdt::Symbol::Flatness);
    CHECK(b->value == "0.02");
    CHECK(b->datums.empty());
    // Not a frame we know.
    CHECK_FALSE(gdt::parseDxfText("plain text"));
    CHECK_FALSE(gdt::parseDxfText("{\\Fgdt;z}%%v1%%v"));
    CHECK_FALSE(gdt::parseDxfText("{\\Fgdt;j}%%v%%v"));
}

TEST_CASE("the table of frames survives writing and reading the header") {
    Header h;
    gdt::FrameTable t;
    gdt::Spec s;
    s.symbol = gdt::Symbol::Position;
    s.value = "0.1";
    s.diameterZone = true;
    s.material = 'L';
    s.datums = {"A", "C"};
    t["TB_TOL_1"] = s;
    gdt::Spec f;
    f.symbol = gdt::Symbol::Flatness;
    f.value = "0.02";
    t["TB_TOL_2"] = f;
    gdt::writeFrames(h, t);
    const gdt::FrameTable back = gdt::readFrames(h);
    CHECK(back == t);
    gdt::writeFrames(h, {});
    CHECK(h.custom.find("tbcad.tolerances") == h.custom.end());
}
