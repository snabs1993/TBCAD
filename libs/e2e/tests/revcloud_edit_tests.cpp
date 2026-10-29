// A revision cloud stays a cloud after it is drawn: its arc length can be changed from the
// properties (-PROP Set cloudarclength), dragging a corner grip or STRETCH lays the arcs again,
// SCALE takes the arc length along, and the cloud is still one after .tbcad and DXF.
#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/geom.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layers/properties.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, reg) { registerAllCommands(reg); }
    EntityId only() const {
        std::vector<EntityId> ids;
        doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
        REQUIRE(ids.size() == 1);
        return ids[0];
    }
    const Entity& cloud() const { return *doc.find(only()); }
    const PolylineData& pl() const { return *entityAs<PolylineData>(cloud()); }
};

// A 100 x 50 cloud with 25 long arcs (12 of them).
void drawCloud(Rig& r) {
    r.host.run("REVC", {"Arclength", "25", "0,0", "100,0", "100,50", "0,50", ""});
}

double longestChord(const PolylineData& p) {
    double best = 0;
    for (std::size_t i = 0; i < p.v.size(); ++i)
        best = std::max(best, distance(p.v[i].p, p.v[(i + 1) % p.v.size()].p));
    return best;
}

}  // namespace

TEST_CASE("the arc length of a drawn cloud shows in the properties and can be changed") {
    Rig r;
    drawCloud(r);
    REQUIRE(r.pl().v.size() == 12);
    const auto fields = layers::geometryFields(r.cloud());
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [](const auto& f) { return f.id == "cloudarclength"; });
    REQUIRE(it != fields.end());
    CHECK(it->editable);
    CHECK(it->value == doctest::Approx(25));

    const std::string before = canonicalDump(r.doc);
    r.host.setSelection({r.only()});
    r.host.run("-PROP", {"Set", "cloudarclength", "10", ScriptItem::enter()});
    CHECK(r.pl().v.size() == 30);
    CHECK(longestChord(r.pl()) <= 10 + 1e-9);
    CHECK(geom::cloudShape(r.cloud())->arcLength == doctest::Approx(10));
    // still the same outline, arcs still outwards
    const auto b = geom::bounds(r.cloud(), r.doc);
    CHECK(b.min.y < 0);
    CHECK(b.max.y > 50);

    r.host.setSelection({r.only()});
    r.host.run("-PROP", {"Set", "cloudinward", "Yes", ScriptItem::enter()});
    CHECK(geom::cloudShape(r.cloud())->inward);
    CHECK(geom::bounds(r.cloud(), r.doc).min.y >= -1e-9);

    // Each change is one undo step.
    CHECK(r.host.undo());
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    // A plain polyline has no cloud rows and refuses the field.
    Rig plain;
    plain.host.run("REC", {"0,0", "100,50"});
    const auto pf = layers::geometryFields(plain.cloud());
    CHECK(
        std::none_of(pf.begin(), pf.end(), [](const auto& f) { return f.id == "cloudarclength"; }));
    const std::string rect = canonicalDump(plain.doc);
    plain.host.setSelection({plain.only()});
    plain.host.run("-PROP", {"Set", "cloudarclength", "10", ScriptItem::enter()});
    CHECK(canonicalDump(plain.doc) == rect);
}

TEST_CASE("dragging a cloud corner grip lays the arcs again along the new outline") {
    Rig r;
    drawCloud(r);
    r.host.setSelection({r.only()});
    r.host.run("GRIPEDIT", {"100,50", "200,50"});
    const auto s = geom::cloudShape(r.cloud());
    REQUIRE(s);
    REQUIRE(s->outline.size() == 4);
    CHECK(distance(s->outline[2], {200, 50}) < 1e-9);
    // The top edge is now 200 long and the right edge slanted: all arcs at most 25 long.
    CHECK(longestChord(r.pl()) <= 25 + 1e-9);
    CHECK(r.pl().v.size() > 12);
    CHECK(r.host.undo());
    CHECK(r.pl().v.size() == 12);
}

TEST_CASE("STRETCH moves cloud corners and lays the arcs again") {
    Rig r;
    drawCloud(r);
    r.host.run("STRETCH", {"90,40", "110,60", "0,0", "50,0"});
    const auto s = geom::cloudShape(r.cloud());
    REQUIRE(s);
    REQUIRE(s->outline.size() == 4);
    CHECK(distance(s->outline[2], {150, 50}) < 1e-9);
    CHECK(distance(s->outline[1], {100, 0}) < 1e-9);
    CHECK(longestChord(r.pl()) <= 25 + 1e-9);
}

TEST_CASE("SCALE takes the cloud's arc length along") {
    Rig r;
    drawCloud(r);
    r.host.setSelection({r.only()});
    r.host.run("SCALE", {"0,0", "2"});
    CHECK(geom::cloudShape(r.cloud())->arcLength == doctest::Approx(50));
}

TEST_CASE("a cloud is still a cloud after .tbcad and DXF") {
    Rig r;
    drawCloud(r);

    auto bytes = io::writeTbcadToMemory(r.doc);
    REQUIRE(bytes.ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(bytes.value(), ioReport);
    REQUIRE(back.ok());
    bool found = false;
    back.value()->forEachEntity([&](const Entity& e) {
        const auto s = geom::cloudShape(e);
        REQUIRE(s);
        CHECK(s->arcLength == doctest::Approx(25));
        found = true;
    });
    CHECK(found);

    dxf::Report rep;
    auto out = dxf::exportDxf(r.doc, {}, rep);
    REQUIRE(out.ok());
    const auto& text = out.value().bytes;
    CHECK(text.find(geom::kCloudApp) != std::string::npos);
    dxf::Report rep2;
    auto in = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()),
        {}, rep2);
    REQUIRE(in.ok());
    found = false;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (kind(e) != EntityKind::Polyline)
            return;
        const auto s = geom::cloudShape(e);
        REQUIRE(s);
        CHECK(s->arcLength == doctest::Approx(25));
        CHECK(s->outline.size() == 4);
        found = true;
    });
    CHECK(found);
}
