// Every drawing object the draw commands make (splines, ellipse arcs, polygons, rounded and
// chamfered rectangles, donuts, revision clouds, wipeouts, rays, construction lines, multilines),
// drawn at the origin and at survey coordinates, then pushed through grips, OFFSET, TRIM and both
// file formats. Nothing may go non-finite, undo must restore the drawing exactly, and the file
// round trips must keep the shape.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/entity.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/mline/mline.h"
#include "tbcad/render/fill.h"
#include "tbcad/transform/grips.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, reg) { registerAllCommands(reg); }
    std::vector<EntityId> ids() const {
        std::vector<EntityId> out;
        doc.forEachEntity([&](const Entity& e) { out.push_back(e.common.id); });
        return out;
    }
};

std::string P(Vec2 o, double x, double y) {
    char b[96];
    std::snprintf(b, sizeof b, "%.6f,%.6f", o.x + x, o.y + y);
    return b;
}

struct Maker {
    const char* name;
    std::function<void(Rig&, Vec2)> make;
    bool editable = true;  // OFFSET and TRIM work on it (a multiline has MLEDIT instead)
};

std::vector<Maker> makers() {
    std::vector<Maker> m;
    m.push_back({"spline fit", [](Rig& r, Vec2 o) {
                     r.host.run("SPL",
                                {P(o, 0, 0), P(o, 30, 40), P(o, 60, 0), P(o, 90, 40), ""});
                 }});
    m.push_back({"spline CV", [](Rig& r, Vec2 o) {
                     r.host.run("SPL", {"M", "CV", P(o, 0, 0), P(o, 30, 40), P(o, 60, 0),
                                        P(o, 90, 40), ""});
                 }});
    m.push_back({"closed spline", [](Rig& r, Vec2 o) {
                     r.host.run("SPL",
                                {P(o, 0, 0), P(o, 50, 10), P(o, 60, 50), P(o, 10, 60), "Close"});
                 }});
    m.push_back({"ellipse arc", [](Rig& r, Vec2 o) {
                     r.host.run("EL", {"Arc", P(o, 0, 0), P(o, 100, 0), "40", "0", "100"});
                 }});
    m.push_back(
        {"ellipse", [](Rig& r, Vec2 o) { r.host.run("EL", {P(o, 0, 0), P(o, 100, 30), "40"}); }});
    m.push_back({"polygon",
                 [](Rig& r, Vec2 o) { r.host.run("POL", {"7", P(o, 50, 50), "", P(o, 80, 50)}); }});
    m.push_back({"rectangle fillet", [](Rig& r, Vec2 o) {
                     r.host.run("REC", {P(o, 0, 0), "Fillet", "10", P(o, 100, 60)});
                 }});
    m.push_back({"rectangle chamfer", [](Rig& r, Vec2 o) {
                     r.host.run("REC", {P(o, 0, 0), "Chamfer", "5", "8", P(o, 100, 60)});
                 }});
    m.push_back(
        {"donut", [](Rig& r, Vec2 o) { r.host.run("DO", {"20", "40", P(o, 50, 50), ""}); }});
    m.push_back({"revision cloud", [](Rig& r, Vec2 o) {
                     r.host.run("REVC", {"A", "15", P(o, 0, 0), P(o, 100, 0), P(o, 100, 50),
                                         P(o, 0, 50), ""});
                 }});
    m.push_back({"wipeout", [](Rig& r, Vec2 o) {
                     r.host.run("WIPEOUT", {"Rectangle", P(o, 0, 0), P(o, 40, 20)});
                 }});
    m.push_back(
        {"xline", [](Rig& r, Vec2 o) { r.host.run("XL", {P(o, 0, 0), P(o, 100, 50), ""}); }});
    m.push_back(
        {"ray", [](Rig& r, Vec2 o) { r.host.run("RAY", {P(o, 0, 0), P(o, 100, 50), ""}); }});
    m.push_back(
        {"text", [](Rig& r, Vec2 o) { r.host.run("TEXT", {P(o, 0, 0), "5", "30", "Hello"}); }});
    m.push_back({"mtext", [](Rig& r, Vec2 o) {
                     r.host.run("MTEXT", {P(o, 0, 50), P(o, 40, 20), "boxed text"});
                 }});
    m.push_back({"linear dimension", [](Rig& r, Vec2 o) {
                     r.host.run("DIMLINEAR", {P(o, 0, 0), P(o, 100, 0), P(o, 50, 10)});
                 }});
    m.push_back({"aligned dimension", [](Rig& r, Vec2 o) {
                     r.host.run("DIMALIGNED", {P(o, 0, 0), P(o, 100, 60), P(o, 40, 50)});
                 }});
    m.push_back({"hatch", [](Rig& r, Vec2 o) {
                     r.host.run("REC", {P(o, 0, 0), P(o, 100, 60)});
                     r.host.run("HATCH", {P(o, 50, 30), ""});
                 }});
    m.push_back({"block insert", [](Rig& r, Vec2 o) {
                     r.host.run("LINE", {P(o, 0, 0), P(o, 10, 0), ""});
                     EntityId line{};
                     r.doc.forEachEntity([&](const Entity& e) { line = e.common.id; });
                     r.host.setSelection({line});
                     r.host.run("BLOCK", {"Mark", P(o, 0, 0), "D"});
                     r.host.run("INSERT", {"Mark", P(o, 50, 20)});
                 }});
    m.push_back(
        {"mline",
         [](Rig& r, Vec2 o) { r.host.run("MLINE", {P(o, 0, 0), P(o, 100, 0), P(o, 100, 50), ""}); },
         false});
    // Annotation is not offset or trimmed (the hatch and block cases test their outline / line).
    for (Maker& mk : m) {
        const std::string n = mk.name;
        if (n == "text" || n == "mtext" || n.find("dimension") != std::string::npos)
            mk.editable = false;
    }
    return m;
}

bool finiteText(const std::string& d) {
    std::string low = d;
    for (auto& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("nan") == std::string::npos && low.find("inf") == std::string::npos;
}

BBox2 sceneBounds(const Document& doc) {
    BBox2 box;
    bool first = true;
    doc.forEachEntity([&](const Entity& e) {
        BBox2 b = geom::bounds(e, doc);
        if (!std::isfinite(b.min.x) || !std::isfinite(b.max.x) || !std::isfinite(b.min.y) ||
            !std::isfinite(b.max.y))
            return;
        if (first) {
            box = b;
            first = false;
        } else {
            box.min = {std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y)};
            box.max = {std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y)};
        }
    });
    return box;
}

const Vec2 kOrigins[] = {{0, 0}, {500123.456, 4500987.654}};

}  // namespace

TEST_CASE("drawing objects exist, stay finite and survive grips, OFFSET and TRIM with exact undo") {
    for (const Maker& mk : makers())
        for (Vec2 o : kOrigins) {
            CAPTURE(mk.name);
            CAPTURE(o.x);
            Rig r;
            mk.make(r, o);
            const auto made = r.ids();
            REQUIRE_MESSAGE(!made.empty(), "the command drew nothing");
            const std::string base = canonicalDump(r.doc);
            CHECK(finiteText(base));
            const BBox2 box = geom::bounds(*r.doc.find(made[0]), r.doc);

            // Grips: every grip stretches to a finite result.
            auto grips = transform::collectGrips(r.doc, made);
            CHECK_MESSAGE(!grips.empty(), "no grips");
            for (const auto& g : grips) {
                auto res = transform::stretchResults(r.doc, grips, g, {7, -3});
                for (const auto& [id, ent] : res)
                    CHECK(finiteText(dumpEntity(ent)));
            }

            // OFFSET to both sides: finite, one undo step each, exact undo.
            for (Vec2 side :
                 {Vec2{box.min.x - 30, box.min.y - 30}, Vec2{box.max.x + 30, box.max.y + 30}}) {
                const auto depth = r.host.undoStack().undoCount();
                r.host.run("OFFSET", {"5", ScriptItem::pick(made[0], box.min),
                                      P({0, 0}, side.x, side.y), ScriptItem::enter()});
                const std::string offsetDump = canonicalDump(r.doc);
                CHECK(finiteText(offsetDump));
                CHECK_MESSAGE(
                    (offsetDump != base) == mk.editable,
                    mk.name << ": OFFSET " << (r.host.log().empty() ? "" : r.host.log().back()));
                while (r.host.undoStack().undoCount() > depth && r.host.undo()) {
                }
                CHECK(canonicalDump(r.doc) == base);
            }

            // TRIM against a line through the middle of the object.
            const double midY = 0.5 * (box.min.y + box.max.y);
            const double lo = std::isfinite(box.min.x) ? box.min.x - 10 : o.x - 10;
            {
                Transaction tx = r.doc.begin("edge");
                REQUIRE(tx.add(Entity{{}, LineData{{lo, midY}, {box.max.x + 10, midY}}}).ok());
                tx.commit();
            }
            const std::string withEdge = canonicalDump(r.doc);
            const auto depth = r.host.undoStack().undoCount();
            r.host.run("TRIM", {ScriptItem::enter(), ScriptItem::pick(made[0], box.min),
                                ScriptItem::enter()});
            const std::string trimDump = canonicalDump(r.doc);
            CHECK(finiteText(trimDump));
            // The block's line lies along the cutting edge, so nothing crosses.
            const bool trimmed = trimDump != withEdge;
            const bool skip = std::string(mk.name) == "block insert";
            const bool trimOk = skip || trimmed == mk.editable;
            CHECK_MESSAGE(trimOk, mk.name << ": TRIM");
            while (r.host.undoStack().undoCount() > depth && r.host.undo()) {
            }
            CHECK(canonicalDump(r.doc) == withEdge);
        }
}

TEST_CASE("drawing objects keep their shape through .tbcad and DXF files at survey coordinates") {
    for (const Maker& mk : makers())
        for (Vec2 o : kOrigins) {
            CAPTURE(mk.name);
            CAPTURE(o.x);
            Rig r;
            mk.make(r, o);
            REQUIRE(!r.ids().empty());
            const std::string dump = canonicalDump(r.doc);

            auto bytes = io::writeTbcadToMemory(r.doc);
            REQUIRE(bytes.ok());
            io::IoReport ioReport;
            auto back = io::readTbcad(bytes.value(), ioReport);
            REQUIRE(back.ok());
            CHECK(canonicalDump(*back.value()) == dump);

            dxf::Report rep;
            auto out = dxf::exportDxf(r.doc, {}, rep);
            REQUIRE(out.ok());
            dxf::Report rep2;
            const auto& s = out.value().bytes;
            auto in = dxf::importDxf(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()),
                {}, rep2);
            REQUIRE(in.ok());
            const Document& d2 = *in.value().document;
            CHECK(d2.modelSpaceCount() > 0);
            const BBox2 a = sceneBounds(r.doc), b = sceneBounds(d2);
            const bool bounded = (a.max.x - a.min.x) < 1e5 && (a.max.y - a.min.y) < 1e5;
            if (bounded) {
                CHECK(std::abs(a.min.x - b.min.x) < 1e-3);
                CHECK(std::abs(a.min.y - b.min.y) < 1e-3);
                CHECK(std::abs(a.max.x - b.max.x) < 1e-3);
                CHECK(std::abs(a.max.y - b.max.y) < 1e-3);
            }
        }
}

namespace {
struct Shape {
    std::size_t count = 0;
    double length = 0;
    Vec2 size{};
};

Shape shapeOf(const Document& doc) {
    Shape s;
    doc.forEachEntity([&](const Entity& e) {
        ++s.count;
        if (kind(e) != EntityKind::Ray)
            s.length += geom::length(e);
    });
    const BBox2 b = sceneBounds(doc);
    s.size = {b.max.x - b.min.x, b.max.y - b.min.y};
    return s;
}
}  // namespace

TEST_CASE("drawing objects are the same shape at survey coordinates as at the origin") {
    for (const Maker& mk : makers()) {
        CAPTURE(mk.name);
        Rig a, b;
        mk.make(a, kOrigins[0]);
        mk.make(b, kOrigins[1]);
        const Shape sa = shapeOf(a.doc), sb = shapeOf(b.doc);
        CHECK(sa.count == sb.count);
        CHECK(std::abs(sa.length - sb.length) <= 1e-6 * std::max(1.0, sa.length));
        CHECK(std::abs(sa.size.x - sb.size.x) < 1e-5);
        CHECK(std::abs(sa.size.y - sb.size.y) < 1e-5);
        if (!mk.editable)
            continue;
        // And after an OFFSET.
        for (Rig* r : {&a, &b}) {
            const EntityId id = r->ids().front();
            const BBox2 box = geom::bounds(*r->doc.find(id), r->doc);
            const Vec2 o = r == &a ? kOrigins[0] : kOrigins[1];
            r->host.run("OFFSET", {"3", ScriptItem::pick(id, box.min),
                                   P({0, 0}, box.max.x + 20, box.max.y + 20), ScriptItem::enter()});
            (void)o;
        }
        const Shape oa = shapeOf(a.doc), ob = shapeOf(b.doc);
        CHECK(oa.count == ob.count);
        CHECK(std::abs(oa.length - ob.length) <= 1e-6 * std::max(1.0, oa.length));
    }
}

TEST_CASE(
    "editing commands on every drawing object at survey coordinates never break the drawing") {
    static const char* const kCommands[] = {
        "MOVE",      "COPY",   "ROTATE",  "SCALE",  "MIRROR",  "STRETCH", "TRIM",  "EXTEND",
        "OFFSET",    "BREAK",  "JOIN",    "FILLET", "CHAMFER", "EXPLODE", "PEDIT", "LENGTHEN",
        "SPLINEDIT", "DIVIDE", "MEASURE", "ARRAY",  "REGION",  "GRIPEDIT"};
    std::mt19937 rng(20261018);
    cmd::Registry probe;
    registerAllCommands(probe);
    for (const char* name : kCommands) {
        if (!probe.find(name))
            continue;
        for (const Maker& mk : makers()) {
            for (int round = 0; round < 1; ++round) {
                CAPTURE(name);
                CAPTURE(mk.name);
                CAPTURE(round);
                Rig r;
                const Vec2 o = kOrigins[1];
                mk.make(r, o);
                const auto ids = r.ids();
                if (ids.empty())
                    continue;
                const std::string base = canonicalDump(r.doc);
                const auto depth = r.host.undoStack().undoCount();
                const BBox2 box = geom::bounds(*r.doc.find(ids[0]), r.doc);
                auto pt = [&](double fx, double fy) {
                    const double w = std::min(box.max.x - box.min.x, 200.0);
                    const double h = std::min(box.max.y - box.min.y, 200.0);
                    return Vec2{box.min.x + fx * w, box.min.y + fy * h};
                };
                static const char* const kWords[] = {"5",   "0",      "-3",   "2",      "Yes",
                                                     "No",  "All",    "Last", "Close",  "Undo",
                                                     "Add", "Delete", "Move", "Fillet", "0.5"};
                std::vector<ScriptItem> items;
                const std::size_t n = 4 + rng() % 8;
                for (std::size_t i = 0; i < n; ++i) {
                    switch (rng() % 6) {
                        case 0:
                            items.push_back(ScriptItem::enter());
                            break;
                        case 1: {
                            Vec2 p = pt((rng() % 120) / 100.0 - 0.1, (rng() % 120) / 100.0 - 0.1);
                            items.push_back(ScriptItem::click(p));
                            break;
                        }
                        case 2:
                            items.push_back(
                                ScriptItem::pick(ids[rng() % ids.size()],
                                                 pt((rng() % 100) / 100.0, (rng() % 100) / 100.0)));
                            break;
                        case 3:
                            items.push_back(ScriptItem::select({ids[rng() % ids.size()]}));
                            break;
                        case 4: {
                            Vec2 p = pt((rng() % 100) / 100.0, (rng() % 100) / 100.0);
                            items.push_back(ScriptItem(P({0, 0}, p.x, p.y)));
                            break;
                        }
                        default:
                            items.push_back(ScriptItem(kWords[rng() % std::size(kWords)]));
                    }
                }
                items.push_back(ScriptItem::esc());
                items.push_back(ScriptItem::esc());
                (void)r.host.run(name, items);
                const std::string after = canonicalDump(r.doc);
                CHECK(finiteText(after));
                while (r.host.undoStack().undoCount() > depth && r.host.undo()) {
                }
                CHECK(canonicalDump(r.doc) == base);
                auto bytes = io::writeTbcadToMemory(r.doc);
                REQUIRE(bytes.ok());
                io::IoReport rep;
                auto back = io::readTbcad(bytes.value(), rep);
                REQUIRE(back.ok());
                CHECK(canonicalDump(*back.value()) == base);
            }
        }
    }
}

namespace {
std::string kindsOf(const Document& doc) {
    std::map<int, int> n;
    doc.forEachEntity([&](const Entity& e) { ++n[static_cast<int>(kind(e))]; });
    std::string out;
    for (auto [k, c] : n)
        out += std::to_string(k) + "x" + std::to_string(c) + " ";
    return out;
}
}  // namespace

TEST_CASE("DXF round trip keeps the kind and the defining numbers of every drawing object") {
    for (const Maker& mk : makers())
        for (Vec2 o : kOrigins) {
            CAPTURE(mk.name);
            CAPTURE(o.x);
            Rig r;
            mk.make(r, o);
            dxf::Report rep;
            auto out = dxf::exportDxf(r.doc, {}, rep);
            REQUIRE(out.ok());
            dxf::Report rep2;
            const auto& s = out.value().bytes;
            auto in = dxf::importDxf(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()),
                {}, rep2);
            REQUIRE(in.ok());
            const Document& d2 = *in.value().document;
            CHECK(kindsOf(r.doc) == kindsOf(d2));
            const Entity* a = nullptr;
            const Entity* b = nullptr;
            r.doc.forEachEntity([&](const Entity& e) { a = &e; });
            REQUIRE(a);
            // The import numbers objects in draw order (a hatch goes behind its boundary), so
            // match by kind.
            d2.forEachEntity([&](const Entity& e) {
                if (kind(e) == kind(*a))
                    b = &e;
            });
            REQUIRE(b);
            if (kind(*a) != EntityKind::Ray)
                CHECK(std::abs(geom::length(*a) - geom::length(*b)) <= 1e-5 * geom::length(*a));
            if (const auto* x = entityAs<RayData>(*a)) {
                const auto* y = entityAs<RayData>(*b);
                REQUIRE(y);
                CHECK(distance(x->origin, y->origin) < 1e-6);
                CHECK(distance(x->dir, y->dir) < 1e-9);
                CHECK(x->infinite == y->infinite);
            } else if (const auto* sp = entityAs<SplineData>(*a)) {
                const auto* y = entityAs<SplineData>(*b);
                REQUIRE(y);
                CHECK(sp->degree == y->degree);
                CHECK(sp->closed == y->closed);
                CHECK(sp->control.size() == y->control.size());
            } else if (const auto* el = entityAs<EllipseData>(*a)) {
                const auto* y = entityAs<EllipseData>(*b);
                REQUIRE(y);
                CHECK(distance(el->center, y->center) < 1e-6);
                CHECK(std::abs(el->ratio - y->ratio) < 1e-9);
                CHECK(std::abs(normalizeAngle(el->startParam) - normalizeAngle(y->startParam)) <
                      1e-9);
                CHECK(std::abs(normalizeAngle(el->endParam) - normalizeAngle(y->endParam)) < 1e-9);
            } else if (const auto* ml = entityAs<MlineData>(*a)) {
                const auto* y = entityAs<MlineData>(*b);
                REQUIRE(y);
                CHECK(ml->v.size() == y->v.size());
                CHECK(ml->style == y->style);
                CHECK(ml->justification == y->justification);
                CHECK(std::abs(ml->scale - y->scale) < 1e-9);
            } else if (const auto* pl = entityAs<PolylineData>(*a)) {
                const auto* y = entityAs<PolylineData>(*b);
                REQUIRE(y);
                CHECK(pl->v.size() == y->v.size());
                CHECK(pl->closed == y->closed);
                CHECK(a->common.wipeout == b->common.wipeout);
                if (std::string(mk.name) == "wipeout")
                    CHECK(a->common.wipeout);
                for (std::size_t i = 0; i < std::min(pl->v.size(), y->v.size()); ++i)
                    CHECK(std::abs(pl->v[i].bulge - y->v[i].bulge) < 1e-9);
            }
        }
}

TEST_CASE("a custom MLSTYLE and the multiline drawn with it survive .tbcad and DXF") {
    for (Vec2 o : kOrigins) {
        CAPTURE(o.x);
        Rig r;
        r.host.run("-MLSTYLE", {"New", "WALL", "1 0 -1", "Both"});
        r.host.run("-MLSTYLE", {"Current", "WALL"});
        r.host.run("MLINE", {"Scale", "50", P(o, 0, 0), P(o, 1000, 0), P(o, 1000, 500), ""});
        const std::string dump = canonicalDump(r.doc);
        auto bytes = io::writeTbcadToMemory(r.doc);
        REQUIRE(bytes.ok());
        io::IoReport rep;
        auto back = io::readTbcad(bytes.value(), rep);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == dump);

        dxf::Report dr;
        auto out = dxf::exportDxf(r.doc, {}, dr);
        REQUIRE(out.ok());
        dxf::Report dr2;
        const auto& s = out.value().bytes;
        auto in = dxf::importDxf(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()), {},
            dr2);
        REQUIRE(in.ok());
        const Document& d2 = *in.value().document;
        const MlineData* m = nullptr;
        d2.forEachEntity([&](const Entity& e) {
            if (const auto* x = entityAs<MlineData>(e))
                m = x;
        });
        REQUIRE(m);
        CHECK(m->style == "WALL");
        CHECK(m->v.size() == 3);
        auto style = mline::findStyle(d2, "WALL");
        REQUIRE(style);
        CHECK(style->offsets.size() == 3);
        CHECK(style->startCap);
        CHECK(style->endCap);
    }
}

TEST_CASE("a DONUT fills its ring on screen: the filled area is the ring's area") {
    for (const double inner : {0.0, 5.0}) {
        CAPTURE(inner);
        Rig r;
        REQUIRE(r.host.run("DONUT", {std::to_string(inner), "10", "100,50", ""}).finished);
        REQUIRE(r.ids().size() == 1);
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = 0.01;
        DisplayList list;
        geom::buildDisplay(r.doc, *r.doc.find(r.ids()[0]), ctx, list);
        double area = 0;
        for (const DisplayItem& item : list.items)
            if (const auto* f = std::get_if<DisplayFill>(&item))
                area += render::triangleArea(render::triangulateEvenOdd(f->rings));
        const double expected = std::numbers::pi * (25 - inner * inner / 4);
        CHECK(area == doctest::Approx(expected).epsilon(0.01));
    }
}
