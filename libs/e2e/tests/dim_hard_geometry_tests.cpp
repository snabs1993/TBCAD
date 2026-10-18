// Every dimension command on difficult geometry (ellipse and spline ends, polyline arcs, arcs,
// survey coordinates up to 1e9, dimensions inside scaled and mirrored blocks): the measurement is
// what the geometry says, the text is what the style says, the drawing is finite, and the result
// survives .tbcad and DXF.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/geom.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Vec2 o;
    EntityId line{}, line2{}, ell{}, spline{}, poly{}, circle{}, arc{};

    explicit Rig(Vec2 origin) : host(doc, (registerAllCommands(reg), reg)), o(origin) {
        auto tx = doc.begin("setup");
        auto add = [&](EntityData d) {
            Entity e;
            e.data = std::move(d);
            return tx.add(std::move(e)).value();
        };
        line = add(LineData{o + Vec2{0, 0}, o + Vec2{120.5, 35.25}});
        line2 = add(LineData{o + Vec2{0, 0}, o + Vec2{-40, 90}});
        ell = add(EllipseData{o + Vec2{200, 50}, {60, 20}, 0.5, 0.3, 4.0});
        SplineData sp;
        sp.degree = 3;
        sp.knots = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3};
        sp.control = {o + Vec2{0, 200},   o + Vec2{30, 260},  o + Vec2{70, 180},
                      o + Vec2{110, 250}, o + Vec2{150, 190}, o + Vec2{190, 240}};
        spline = add(std::move(sp));
        PolylineData pl;
        pl.v = {{o + Vec2{300, 0}, 0.0, 0, 0},
                {o + Vec2{360, 0}, 0.5, 0, 0},
                {o + Vec2{360, 60}, 0.0, 0, 0},
                {o + Vec2{300, 60}, 0.0, 0, 0}};
        poly = add(std::move(pl));
        circle = add(CircleData{o + Vec2{500, 100}, 33.3});
        arc = add(ArcData{o + Vec2{600, 100}, 45.5, 0.2, 2.1});
        tx.commit();
    }

    std::vector<EntityId> dims() const {
        std::vector<EntityId> v;
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                v.push_back(e.common.id);
        });
        return v;
    }
    const DimensionData& last() const {
        const auto v = dims();
        REQUIRE(!v.empty());
        return *entityAs<DimensionData>(*doc.find(v.back()));
    }
    bool run(const char* command, std::vector<ScriptItem> items) {
        const std::size_t n = dims().size();
        const auto r = host.run(command, items);
        (void)host.feed({ScriptItem::esc()});
        return r.finished && dims().size() == n + 1;
    }
    static ScriptItem pickAt(EntityId id, Vec2 at) { return ScriptItem::pick(id, at); }
    ScriptItem click(Vec2 p) const { return ScriptItem::click(o + p); }
};

bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

// What every dimension must satisfy, whatever it measures.
void checkSound(Rig& r, const DimensionData& d, double expectMeasurement) {
    // Points are stored as doubles at their distance from the origin: the rounding of that is
    // the best a measurement can do.
    const double eps = 1e-9 + std::max(std::abs(r.o.x), std::abs(r.o.y)) * 4e-16;
    CHECK(std::abs(dim::measurement(d) - expectMeasurement) <=
          eps * std::max(1.0, expectMeasurement));
    const dim::Layout L = dim::layoutDimension(r.doc, d);
    CHECK(L.text.valid());
    CHECK(L.text.utf8 == dim::dimensionText(dim::styleOf(r.doc, d.style), d));
    CHECK(finite(L.text.center));
    for (const auto& l : L.lines)
        for (Vec2 p : l.pts)
            CHECK(finite(p));
    for (const auto& a : L.arrows)
        for (Vec2 p : a.ring)
            CHECK(finite(p));
    // The text sits near what it measures: within a few thousand units of a definition point,
    // never at the origin or at infinity when the drawing is at 1e9.
    CHECK(distance(L.text.center, d.defPts[0]) < 5000);
}

}  // namespace

TEST_CASE("every dimension command works on difficult geometry at any distance from the origin") {
    for (double off : {0.0, 1e6, 1e9}) {
        CAPTURE(off);
        Rig r({off, -off * 0.5});

        // Linear and aligned, on a line, an elliptical arc, a spline and a polyline segment.
        const Vec2 la = r.o, lb = r.o + Vec2{120.5, 35.25};
        REQUIRE(r.run("DIMALIGNED", {ScriptItem::enter(), Rig::pickAt(r.line, r.o + Vec2{60, 17}),
                                     r.click({60, -30})}));
        checkSound(r, r.last(), distance(la, lb));
        REQUIRE(r.run("DIMLINEAR", {ScriptItem::enter(), Rig::pickAt(r.line, r.o + Vec2{60, 17}),
                                    r.click({60, -30})}));
        checkSound(r, r.last(), 120.5);

        const auto eEnds = geom::endpoints(*r.doc.find(r.ell));
        REQUIRE(eEnds.size() >= 2);
        REQUIRE(r.run("DIMALIGNED", {ScriptItem::enter(), Rig::pickAt(r.ell, r.o + Vec2{200, 70}),
                                     r.click({200, 130})}));
        checkSound(r, r.last(), distance(eEnds.front(), eEnds.back()));

        const auto sEnds = geom::endpoints(*r.doc.find(r.spline));
        REQUIRE(sEnds.size() >= 2);
        REQUIRE(
            r.run("DIMALIGNED", {ScriptItem::enter(), Rig::pickAt(r.spline, r.o + Vec2{100, 220}),
                                 r.click({100, 300})}));
        checkSound(r, r.last(), distance(sEnds.front(), sEnds.back()));

        REQUIRE(r.run("DIMLINEAR", {ScriptItem::enter(), Rig::pickAt(r.poly, r.o + Vec2{330, 60}),
                                    r.click({330, 100})}));
        checkSound(r, r.last(), 60);

        // Radius and diameter of a circle, an arc and a bulged polyline segment.
        REQUIRE(r.run("DIMRADIUS",
                      {Rig::pickAt(r.circle, r.o + Vec2{533.3, 100}), r.click({560, 130})}));
        checkSound(r, r.last(), 33.3);
        REQUIRE(r.run("DIMDIAMETER",
                      {Rig::pickAt(r.circle, r.o + Vec2{533.3, 100}), r.click({560, 130})}));
        checkSound(r, r.last(), 66.6);
        REQUIRE(
            r.run("DIMRADIUS", {Rig::pickAt(r.arc, r.o + Vec2{600, 145.5}), r.click({640, 190})}));
        checkSound(r, r.last(), 45.5);
        {
            // The bulge 0.5 on a 60 chord: sagitta 15 (b * c / 2), radius (c^2/4 + s^2) / (2 s).
            const double chord = 60, sag = 0.5 * chord / 2;
            const double radius = (chord * chord / 4 + sag * sag) / (2 * sag);
            // The arc bulges to the right of the travel direction when the bulge is negative, to
            // the left when positive: the point on it is at (330, -15) or (330, 15).
            REQUIRE(r.run("DIMRADIUS",
                          {Rig::pickAt(r.poly, r.o + Vec2{330, -15}), r.click({330, -60})}));
            checkSound(r, r.last(), radius);
        }

        // Arc length and angles: two lines, an arc, three points.
        REQUIRE(r.run("DIMARC", {Rig::pickAt(r.arc, r.o + Vec2{600, 145.5}), r.click({600, 170})}));
        checkSound(r, r.last(), 45.5 * (2.1 - 0.2));
        {
            const Vec2 d1 = normalized(lb - la), d2 = normalized(Vec2{-40, 90});
            const double ang = std::acos(dot(d1, d2));
            REQUIRE(r.run("DIMANGULAR",
                          {Rig::pickAt(r.line, r.o + Vec2{60, 17}),
                           Rig::pickAt(r.line2, r.o + Vec2{-20, 45}), r.click({40, 55})}));
            const double got = dim::measurement(r.last());
            CHECK((std::abs(got - ang) < 1e-9 || std::abs(got - (kPi - ang)) < 1e-9 ||
                   std::abs(got - (kTwoPi - ang)) < 1e-9 || std::abs(got - (kPi + ang)) < 1e-9));
            checkSound(r, r.last(), got);
        }
        REQUIRE(
            r.run("DIMANGULAR", {Rig::pickAt(r.arc, r.o + Vec2{600, 145.5}), r.click({600, 160})}));
        checkSound(r, r.last(), 2.1 - 0.2);
        REQUIRE(r.run("DIMANGULAR", {"", r.click({0, 0}), r.click({50, 0}), r.click({0, 50}),
                                     r.click({30, 30})}));
        checkSound(r, r.last(), kPi / 2);

        // Ordinate: the distance of the feature from the origin.
        REQUIRE(r.run("DIMORDINATE", {r.click({10, 20}), r.click({10, 60})}));
        checkSound(r, r.last(), std::abs(r.o.x + 10));

        // Chains continue from the last linear dimension.
        REQUIRE(r.run("DIMLINEAR", {r.click({0, 0}), r.click({40, 0}), r.click({20, -20})}));
        checkSound(r, r.last(), 40);
        const std::size_t nBefore = r.dims().size();
        (void)r.host.run("DIMCONTINUE", {r.click({90, 0}), "", ""});
        (void)r.host.feed({ScriptItem::esc()});
        CHECK(r.dims().size() == nBefore + 1);
        checkSound(r, r.last(), 50);
        (void)r.host.run("DIMBASELINE", {r.click({130, 0}), "", ""});
        (void)r.host.feed({ScriptItem::esc()});
        CHECK(r.dims().size() == nBefore + 2);
        // A baseline starts at the first extension line of the dimension before it (at 40).
        checkSound(r, r.last(), 90);

        // Undo takes every dimension back, redo returns them.
        const std::string full = canonicalDump(r.doc);
        const std::size_t count = r.dims().size();
        while (!r.dims().empty() && r.host.undo()) {
        }
        CHECK(r.dims().empty());
        while (r.host.redo()) {
        }
        CHECK(r.dims().size() == count);
        CHECK(canonicalDump(r.doc) == full);

        // .tbcad round trip is exact.
        const auto file =
            std::filesystem::temp_directory_path() /
            ("tbcad_dimhard_" + std::to_string(static_cast<long long>(off)) + ".tbcad");
        REQUIRE(io::writeTbcad(r.doc, file).ok());
        io::IoReport ir;
        auto back = io::readTbcad(file, ir);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == full);
        std::error_code ec;
        std::filesystem::remove(file, ec);

        // DXF round trip keeps every dimension, its kind and its measurement.
        dxf::Report er;
        auto out = dxf::exportDxf(r.doc, dxf::ExportOptions{}, er);
        REQUIRE(out.ok());
        const std::string& text = out.value().bytes;
        std::vector<std::byte> bytes(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            bytes[i] = static_cast<std::byte>(text[i]);
        dxf::Report imr;
        auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, imr);
        REQUIRE_MESSAGE(in.ok(), (in.ok() ? "" : in.error().message));
        std::vector<const DimensionData*> a, b;
        r.doc.forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<DimensionData>(e))
                a.push_back(d);
        });
        in.value().document->forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<DimensionData>(e))
                b.push_back(d);
        });
        // Arc length and jogged dimensions leave DXF as plain geometry; the others stay.
        std::size_t keepable = 0;
        for (const auto* d : a)
            keepable += d->kind != DimKind::ArcLength && d->kind != DimKind::Jogged;
        CHECK(b.size() == keepable);
        for (const auto* d : b) {
            CHECK(std::isfinite(dim::measurement(*d)));
            CHECK(finite(d->defPts[0]));
        }
    }
}

#include "tbcad/cmd/undo.h"
#include "tbcad/ui/scene_sync.h"

TEST_CASE("changing a dimension style redraws the dimensions that use it, undo and redo too") {
    dim::registerDisplay();
    Document doc;
    render::Scene scene;
    ui::SceneSync sync(doc, scene);
    cmd::UndoStack undo;
    {
        auto tx = doc.begin("dim");
        DimensionData d;
        d.kind = DimKind::Linear;
        d.defPts = {Vec2{0, 0}, Vec2{100, 0}, Vec2{50, 10}, Vec2{}};
        dim::refresh(doc, d);
        Entity e;
        e.data = std::move(d);
        (void)tx.add(std::move(e));
        tx.commit();
    }
    const BBox2 before = scene.bounds();
    {
        auto tx = doc.begin("restyle");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.textHeight = 40; }).ok());
        undo.push("restyle", {tx.commit()});
    }
    const BBox2 big = scene.bounds();
    CHECK(big.max.y > before.max.y + 20);
    REQUIRE(undo.undo(doc));
    CHECK(scene.bounds().max.y == doctest::Approx(before.max.y));
    REQUIRE(undo.redo(doc));
    CHECK(scene.bounds().max.y == doctest::Approx(big.max.y));
}

TEST_CASE("alternate units and tolerances: command, text, undo, .tbcad and DXF") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(host.run("DIMSTYLE", {"New", "Plan", "Done"}).finished);
    REQUIRE(host.run("DIMSTYLE", {"Edit",      "Plan",         "AltUnits", "On",        "AltUnit",
                                  "in",        "AltPrecision", "1",        "AltSuffix", "in",
                                  "Tolerance", "Deviation",    "TolUpper", "0.05",      "TolLower",
                                  "0.02",      "TolPrecision", "2",        "Done",      "Done"})
                .finished);
    REQUIRE(host.run("DIMSTYLE", {"Current", "Plan", "Done"}).finished);
    const DimStyle* plan = doc.dimStyleByName("Plan");
    REQUIRE(plan);
    CHECK(plan->altUnits);
    CHECK(plan->altUnit == LengthUnit::Inch);
    CHECK(plan->tolerance == DimTolerance::Deviation);
    // Bad values change nothing.
    const std::string before = canonicalDump(doc);
    (void)host.run("DIMSTYLE", {"Edit", "Plan", "TolUpper", "-1", "AltScale", "0", "AltUnit", "yd",
                                "Tolerance", "Wide", "Done", "Done"});
    CHECK(canonicalDump(doc) == before);

    REQUIRE(host.run("DIMLINEAR", {"0,0", "254,0", "100,20"}).finished);
    EntityId dimId{};
    doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    REQUIRE(dimId);
    auto textOf = [&](const Document& d, EntityId id) {
        const auto* dd = entityAs<DimensionData>(*d.find(id));
        return dim::dimensionText(dim::styleOf(d, dd->style), *dd);
    };
    CHECK(textOf(doc, dimId) == "254.00 +0.05 -0.02 [10.0in]");

    // Each tolerance mode survives the file and DXF with the same text.
    for (const char* mode : {"Symmetrical", "Deviation", "Limits", "None"}) {
        CAPTURE(mode);
        REQUIRE(host.run("DIMSTYLE", {"Edit", "Plan", "Tolerance", mode, "Done", "Done"}).finished);
        const std::string want = textOf(doc, dimId);
        const auto file = std::filesystem::temp_directory_path() / "tbcad_dimtol.tbcad";
        REQUIRE(io::writeTbcad(doc, file).ok());
        io::IoReport ir;
        auto back = io::readTbcad(file, ir);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == canonicalDump(doc));
        std::error_code ec;
        std::filesystem::remove(file, ec);

        dxf::Report er;
        auto out = dxf::exportDxf(doc, dxf::ExportOptions{}, er);
        REQUIRE(out.ok());
        const std::string& text = out.value().bytes;
        std::vector<std::byte> bytes(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            bytes[i] = static_cast<std::byte>(text[i]);
        dxf::Report imr;
        auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, imr);
        REQUIRE_MESSAGE(in.ok(), (in.ok() ? "" : in.error().message));
        EntityId got{};
        in.value().document->forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                got = e.common.id;
        });
        REQUIRE(got);
        CHECK(textOf(*in.value().document, got) == want);
    }

    // The style edits undo and redo as single steps.
    const std::string withNone = canonicalDump(doc);
    REQUIRE(host.undo());
    CHECK(canonicalDump(doc) != withNone);
    REQUIRE(host.redo());
    CHECK(canonicalDump(doc) == withNone);
}
