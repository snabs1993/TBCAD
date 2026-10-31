// AutoCAD's per-viewport layer overrides in a DWG: layer extension dictionary -> XRECORD data ->
// our VIEWPORT XDATA "ovr=".
#include <doctest/doctest.h>

#include <string>

#include "map/emit.h"
#include "objects/model.h"

namespace tbcad::dwg::test {
using namespace detail;

namespace {

void putU16(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void putU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF));
}
void putU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int k = 0; k < 8; ++k)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF));
}
// A string as AutoCAD writes it before R2007: RS length, RC code page, the bytes.
void putText(std::vector<std::uint8_t>& b, const std::string& s) {
    putU16(b, static_cast<unsigned>(s.size()));
    b.push_back(30);
    b.insert(b.end(), s.begin(), s.end());
}

}  // namespace

TEST_CASE("XRECORD data bytes are read as groups (strings, handles, longs)") {
    std::vector<std::uint8_t> b;
    putU16(b, 102);
    putText(b, "{ADSK_LYR_COLOR_OVERRIDE");
    putU16(b, 335);
    putU64(b, 0x3C);
    putU16(b, 420);
    putU32(b, 0xC3000005u);
    putU16(b, 102);
    putText(b, "}");
    const std::vector<Group> g = parseXrecordData(b, false);
    REQUIRE(g.size() == 4);
    CHECK(g[0].code == 102);
    CHECK(std::get<std::string>(g[0].value.v) == "{ADSK_LYR_COLOR_OVERRIDE");
    CHECK(std::get<long long>(g[1].value.v) == 0x3C);
    CHECK(static_cast<std::uint32_t>(std::get<long long>(g[2].value.v)) == 0xC3000005u);
    CHECK(std::get<std::string>(g[3].value.v) == "}");

    // R2007+: RS length and UTF-16 characters.
    std::vector<std::uint8_t> u;
    putU16(u, 102);
    putU16(u, 2);
    putU16(u, '{');
    putU16(u, 'A');
    putU16(u, 91);
    putU32(u, 50);
    const std::vector<Group> w = parseXrecordData(u, true);
    REQUIRE(w.size() == 2);
    CHECK(std::get<std::string>(w[0].value.v) == "{A");
    CHECK(std::get<long long>(w[1].value.v) == 50);

    // Cut short: what was whole is kept.
    b.resize(b.size() - 3);
    CHECK(parseXrecordData(b, false).size() == 3);
}

TEST_CASE("a viewport's layer overrides from the layer XRECORDs become VIEWPORT XDATA") {
    detail::Drawing d;
    detail::BlockRec paper;
    paper.handle = 1;
    paper.name = "*Paper_Space";
    d.blocks[1] = paper;
    LinetypeRec dashed;
    dashed.handle = 0x15;
    dashed.name = "DASHED";
    d.linetypes[0x15] = dashed;
    LayerRec walls;
    walls.handle = 0x2A;
    walls.name = "Walls";
    walls.xdic = 0x40;
    d.layers[0x2A] = walls;
    d.dictionaries[0x40] = {{"ADSK_XREC_LAYER_COLOR_OVR", 0x41},
                            {"ADSK_XREC_LAYER_LINETYPE_OVR", 0x42},
                            {"ADSK_XREC_LAYER_LINEWT_OVR", 0x43},
                            {"ADSK_XREC_LAYER_TRANSPARENCY_OVR", 0x44}};
    auto pair = [](long long vp, int code, long long value) {
        return std::vector<Group>{{102, Value{std::string("{ADSK_LYR")}},
                                  {335, Value{vp}},
                                  {code, Value{value}},
                                  {102, Value{std::string("}")}}};
    };
    d.xrecords[0x41] = pair(0x3C, 420, static_cast<long long>(0xC2112233u));
    auto other = pair(0x3D, 420, static_cast<long long>(0xC3000003u));
    d.xrecords[0x41].insert(d.xrecords[0x41].end(), other.begin(), other.end());
    d.xrecords[0x42] = pair(0x3C, 343, 0x15);
    d.xrecords[0x43] = pair(0x3C, 91, 35);
    d.xrecords[0x44] = pair(0x3C, 440, static_cast<long long>(0x02000000u | 127));
    EntityRec vp;
    vp.handle = 0x3C;
    vp.dxfType = "VIEWPORT";
    vp.owner = 1;
    vp.entmode = 0;
    d.entities.push_back(vp);
    const std::string text = detail::emitDxf(d);
    CHECK(text.find("ovr=Walls\t\tr17,34,51\tDASHED\t35\t50\t") != std::string::npos);
    // The other viewport's colour is not this one's.
    CHECK(text.find("i3") == std::string::npos);
}

}  // namespace tbcad::dwg::test
