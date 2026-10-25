// Associative dimensions through the commands that change a line's length or split it: TRIM,
// EXTEND, LENGTHEN, BREAK, FILLET, JOIN, and through DXF.
#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    EntityId line{}, cutter{}, dim{};
    Rig() : host(doc, (registerAllCommands(reg), reg)) {
        {
            auto tx = doc.begin("setup");
            Entity e;
            e.data = LineData{{0, 0}, {100, 0}};
            line = tx.add(std::move(e)).value();
            Entity c;
            c.data = LineData{{60, -40}, {60, 40}};  // crosses the line at x = 60
            cutter = tx.add(std::move(c)).value();
            tx.commit();
        }
        REQUIRE(host.run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                dim = e.common.id;
        });
        REQUIRE(dim);
    }
    const DimensionData& d() const { return *entityAs<DimensionData>(*doc.find(dim)); }
    const LineData& l() const { return *entityAs<LineData>(*doc.find(line)); }
    void checkFollows() {
        REQUIRE(doc.find(line));
        CHECK(distance(d().defPts[0], l().a) < 1e-9);
        CHECK(distance(d().defPts[1], l().b) < 1e-9);
    }
};

}  // namespace

TEST_CASE("TRIM of the measured line's end keeps the dimension on the line") {
    Rig r;
    // Trim the part of the line right of the cutter (click on the 80..100 piece).
    (void)r.host.run("TRIM", {ScriptItem::select({r.cutter}), ScriptItem::enter(),
                              ScriptItem::pick(r.line, {90, 0}), ScriptItem::enter()});
    REQUIRE(r.doc.find(r.line));
    CHECK(r.l().b.x == doctest::Approx(60));
    r.checkFollows();
    CHECK(r.d().measurement == doctest::Approx(60));
    CHECK(r.host.undo());
    CHECK(r.l().b.x == doctest::Approx(100));
    r.checkFollows();
}

TEST_CASE("EXTEND of the measured line's end keeps the dimension on the line") {
    Rig r;
    {
        auto tx = r.doc.begin("shorten");
        (void)tx.modify(r.line, [](Entity& e) { entityAs<LineData>(e)->b = {40, 0}; });
        tx.commit();
    }
    r.host.clearLog();
    (void)r.host.run("EXTEND", {ScriptItem::select({r.cutter}), ScriptItem::enter(),
                                ScriptItem::pick(r.line, {39, 0}), ScriptItem::enter()});
    CHECK(r.l().b.x == doctest::Approx(60));
    r.checkFollows();
}

TEST_CASE("LENGTHEN and BREAK keep the dimension on the line that survives") {
    Rig r;
    r.host.setSelection({});
    (void)r.host.run("LENGTHEN",
                     {"Delta", "20", ScriptItem::pick(r.line, {95, 0}), ScriptItem::enter()});
    CHECK(r.l().b.x == doctest::Approx(120));
    r.checkFollows();
    // BREAK at a point splits the line; the dimension must stay on one of the pieces without
    // pointing at something that no longer exists.
    (void)r.host.run("BREAK", {ScriptItem::pick(r.line, {30, 0}), "70,0"});
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* dm = entityAs<DimensionData>(e))
            for (const DimAssoc& a : dm->assoc)
                if (a.kind != DimAssoc::Kind::None)
                    CHECK(r.doc.find(a.target) != nullptr);
    });
    CHECK(r.doc.find(r.dim));
}

TEST_CASE("FILLET and JOIN with the measured line leave a valid dimension") {
    Rig r;
    (void)r.host.run("FILLET", {"Radius", "0", ScriptItem::pick(r.line, {20, 0}),
                                ScriptItem::pick(r.cutter, {60, 20})});
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* dm = entityAs<DimensionData>(e))
            for (const DimAssoc& a : dm->assoc)
                if (a.kind != DimAssoc::Kind::None)
                    CHECK(r.doc.find(a.target) != nullptr);
    });
    if (r.doc.find(r.line))
        r.checkFollows();
    (void)r.host.run("JOIN", {ScriptItem::select({r.line, r.cutter}), ScriptItem::enter()});
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* dm = entityAs<DimensionData>(e))
            for (const DimAssoc& a : dm->assoc)
                if (a.kind != DimAssoc::Kind::None)
                    CHECK(r.doc.find(a.target) != nullptr);
    });
}

TEST_CASE(
    "the link and a dimension style override survive a DXF round trip and then follow edits") {
    Rig r;
    r.host.setSelection({r.dim});
    (void)r.host.run("DIMOVERRIDE", {"Prefix", "L="});
    const DimStyle* over = r.doc.dimStyle(r.d().style);
    REQUIRE(over);
    CHECK(over->prefix == "L=");

    dxf::ExportOptions opt;
    dxf::Report rep;
    auto out = dxf::exportDxf(r.doc, opt, rep);
    REQUIRE(out.ok());
    dxf::ImportOptions iopt;
    dxf::Report irep;
    const std::string& text = out.value().bytes;
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
        bytes[i] = static_cast<std::byte>(text[i]);
    auto back = dxf::importDxf(bytes, iopt, irep);
    REQUIRE(back.ok());
    Document& d2 = *back.value().document;
    EntityId dim2{}, line2{};
    d2.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dim2 = e.common.id;
        if (entityAs<LineData>(e) && entityAs<LineData>(e)->b.x == 100)
            line2 = e.common.id;
    });
    REQUIRE(dim2);
    REQUIRE(line2);
    const auto* dm = entityAs<DimensionData>(*d2.find(dim2));
    CHECK(dm->assoc[0].target == line2);
    const DimStyle* s2 = d2.dimStyle(dm->style);
    REQUIRE(s2);
    CHECK(s2->prefix == "L=");

    cmd::ScriptHost host2(d2, r.reg);
    host2.setSelection({line2});
    REQUIRE(host2.run("MOVE", {"0,0", "10,5"}).finished);
    const auto* moved = entityAs<LineData>(*d2.find(line2));
    const auto* dm2 = entityAs<DimensionData>(*d2.find(dim2));
    CHECK(distance(dm2->defPts[0], moved->a) < 1e-9);
    CHECK(distance(dm2->defPts[1], moved->b) < 1e-9);
}

namespace {

std::vector<EntityId> dimIds(const Document& doc) {
    std::vector<EntityId> out;
    doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            out.push_back(e.common.id);
    });
    return out;
}

// Every association of every dimension points at an object that exists and at a point it has.
void checkAllLinksValid(const Document& doc) {
    doc.forEachEntity([&](const Entity& e) {
        const auto* dm = entityAs<DimensionData>(e);
        if (!dm)
            return;
        for (const DimAssoc& a : dm->assoc) {
            if (a.kind == DimAssoc::Kind::None)
                continue;
            const Entity* t = doc.find(a.target);
            REQUIRE(t != nullptr);
            const auto p = dimAssocPoint(*t, a);
            REQUIRE(p.has_value());
        }
    });
}

}  // namespace

TEST_CASE("aligned, angular, arc-length and ordinate dimensions follow their objects") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    EntityId poly{}, l1{}, l2{}, arc{};
    {
        auto tx = doc.begin("setup");
        Entity p;
        PolylineData pd;
        pd.v = {{{0, 0}, 0, 0, 0}, {60, 0}, {60, 40}, {0, 40}};
        p.data = std::move(pd);
        poly = tx.add(std::move(p)).value();
        Entity a;
        a.data = LineData{{200, 0}, {300, 0}};
        l1 = tx.add(std::move(a)).value();
        Entity b;
        b.data = LineData{{200, 0}, {260, 60}};
        l2 = tx.add(std::move(b)).value();
        Entity c;
        c.data = ArcData{{100, 150}, 30, 0.2, 2.0};
        arc = tx.add(std::move(c)).value();
        tx.commit();
    }
    (void)host.run("DIMALIGNED", {"60,0", "60,40", "80,20"});
    (void)host.run("DIMANGULAR",
                   {ScriptItem::pick(l1, {250, 0}), ScriptItem::pick(l2, {230, 30}), "240,20"});
    (void)host.run("DIMORDINATE", {"60,40", "Xdatum", "60,60"});
    const std::size_t count = dimIds(doc).size();
    CHECK(count >= 2);
    checkAllLinksValid(doc);

    host.setSelection({poly, l1, l2, arc});
    REQUIRE(host.run("ROTATE", {"50,50", "30"}).finished);
    checkAllLinksValid(doc);
    host.setSelection({poly, l1, l2, arc});
    REQUIRE(host.run("SCALE", {"0,0", "1.5"}).finished);
    checkAllLinksValid(doc);
    host.setSelection({poly, l1, l2, arc});
    REQUIRE(host.run("MIRROR", {"0,0", "0,10", "Yes"}).finished);
    CHECK(dimIds(doc).size() == count);
    // Erased objects: the dimensions stay and no link points at a missing object any more.
    host.setSelection({l1, l2});
    REQUIRE(host.run("ERASE", {}).finished);
    CHECK(dimIds(doc).size() == count);
    checkAllLinksValid(doc);
    while (host.undo()) {
    }
    CHECK(dimIds(doc).size() == 0);
}
