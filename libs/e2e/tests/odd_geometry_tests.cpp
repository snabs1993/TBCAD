// Editing commands on awkward drawings: zero-length and coincident objects, coordinates near 1e9
// and tiny ones near 1e-9, a self-crossing polyline, a closed spline, empty and deeply nested
// blocks. A command may refuse or report an error, but it must not crash, hang or leave a drawing
// that fails to save and load (run under the ASan/UBSan preset to catch memory errors).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

#if defined(__SANITIZE_ADDRESS__)
#define TBCAD_ODD_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TBCAD_ODD_SANITIZED 1
#endif
#endif
#ifdef TBCAD_ODD_SANITIZED
constexpr double kSlowest = 120.0;  // ASan/UBSan builds run several times slower
#else
constexpr double kSlowest = 20.0;
#endif

namespace {

Entity make(auto data) {
    Entity e;
    e.data = std::move(data);
    return e;
}

Entity line(Vec2 a, Vec2 b) {
    return make(LineData{a, b});
}

Entity poly(std::vector<Vec2> pts, bool closed, double bulge = 0) {
    PolylineData p;
    for (Vec2 v : pts)
        p.v.push_back({v, bulge, 0, 0});
    p.closed = closed;
    return make(std::move(p));
}

std::vector<EntityId> buildOddDrawing(Document& doc) {
    std::vector<EntityId> ids;
    auto tx = doc.begin("odd");
    auto add = [&](Entity e, BlockId b = {}) {
        auto r = tx.add(std::move(e), b);
        if (r.ok())
            ids.push_back(r.value());
    };
    add(line({0, 0}, {0, 0}));             // zero length
    add(line({5, 5}, {5, 5}));             // zero length, elsewhere
    add(line({0, 0}, {100, 0}));           // normal
    add(line({100, 0}, {100, 0.0}));       // coincident end point, zero length
    add(line({0, 0}, {100, 0}));           // exact duplicate
    add(line({0, 0}, {1e-9, 1e-9}));       // tiny
    add(line({-1e9, -1e9}, {1e9, 1e9}));   // huge
    add(line({1e9, 0}, {1e9 + 1, 0}));     // far away and short
    add(line({50, -50}, {50, 50}));        // crosses the duplicates
    add(make(CircleData{{50, 0}, 0}));     // zero radius
    add(make(CircleData{{50, 0}, 1e-9}));  // tiny radius
    add(make(CircleData{{50, 0}, 1e9}));   // huge radius
    add(make(CircleData{{50, 0}, 30}));
    add(make(ArcData{{0, 0}, 40, 0, 0}));  // zero sweep
    add(make(ArcData{{0, 0}, 40, 0, 2 * kPi}));
    add(poly({{0, 0}, {100, 100}, {100, 0}, {0, 100}}, true));  // bow tie
    add(poly({{0, 0}, {0, 0}, {0, 0}}, true));                  // all points coincide
    add(poly({{0, 0}, {10, 0}}, false, 1e9));                   // huge bulge
    add(poly({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true, 1));
    {
        SplineData s;
        s.degree = 3;
        s.control = {{0, 0}, {20, 40}, {60, 40}, {80, 0}, {60, -40}, {20, -40}, {0, 0}};
        s.closed = true;
        s.periodic = true;
        for (int i = 0; i < 11; ++i)
            s.knots.push_back(i);
        add(make(std::move(s)));
    }
    {
        SplineData s;  // damaged: too few control points for its degree
        s.degree = 3;
        s.control = {{0, 0}, {1, 1}};
        s.knots = {0, 0, 1, 1};
        add(make(std::move(s)));
    }
    // An empty block and a chain of blocks nested 20 deep, the first five levels hold two inserts
    // of the one below.
    BlockId empty = tx.addBlock({.name = "EMPTY"}).value();
    add(make(InsertData{empty, {10, 10}, {1, 1}, 0}));
    BlockId prev = tx.addBlock({.name = "LEAF"}).value();
    add(line({0, 0}, {10, 10}), prev);
    add(make(CircleData{{5, 5}, 3}), prev);
    for (int i = 0; i < 20; ++i) {
        BlockId b = tx.addBlock({.name = "NEST" + std::to_string(i)}).value();
        add(make(InsertData{prev, {1, 1}, {1, 1}, 0.1}), b);
        if (i < 5)  // doubling at every level would be 2^20 leaves; a few levels are enough
            add(make(InsertData{prev, {-1, 1}, {1, -1}, 0}), b);
        prev = b;
    }
    add(make(InsertData{prev, {200, 200}, {1, 1}, 0}));
    {
        HatchData h;  // a hatch with an empty loop and a zero-area loop
        HatchLoop none;
        h.loops.push_back(none);
        HatchLoop flat;
        flat.edges.push_back(HatchLineEdge{{0, 0}, {10, 0}});
        flat.edges.push_back(HatchLineEdge{{10, 0}, {0, 0}});
        h.loops.push_back(flat);
        add(make(std::move(h)));
    }
    tx.commit();
    return ids;
}

std::vector<ScriptItem> script(std::mt19937& rng, const std::vector<EntityId>& ids, int mode) {
    static const char* const kText[] = {"0,0", "50,0", "100,100", "@10,0", "1e9", "1e-9", "0",
                                        "-5",  "5",    "All",     "Yes",   "No",  "Last", "Close"};
    std::vector<ScriptItem> out;
    auto some = [&] { return ids[rng() % ids.size()]; };
    switch (mode) {
        case 0:  // pick two objects, then a point and an answer
            out = {ScriptItem::pick(some(), {1, 0}), ScriptItem::pick(some(), {1, 0}),
                   ScriptItem::click({25, 25}), ScriptItem::enter(), ScriptItem::enter()};
            break;
        case 1:  // all objects selected, points inside the drawing
            out = {ScriptItem::select(ids),     ScriptItem::enter(), ScriptItem::click({10, 0}),
                   ScriptItem::click({60, 20}), ScriptItem::enter(), ScriptItem::enter()};
            break;
        default:
            for (int i = 0; i < 4 + static_cast<int>(rng() % 8); ++i) {
                switch (rng() % 5) {
                    case 0:
                        out.push_back(ScriptItem::pick(some(), {double(rng() % 100), 0}));
                        break;
                    case 1:
                        out.push_back(
                            ScriptItem::click({double(rng() % 200) - 50, double(rng() % 200)}));
                        break;
                    case 2:
                        out.push_back(ScriptItem::select({some(), some()}));
                        break;
                    case 3:
                        out.push_back(ScriptItem::enter());
                        break;
                    default:
                        out.push_back(ScriptItem(kText[rng() % std::size(kText)]));
                }
            }
    }
    out.push_back(ScriptItem::esc());
    out.push_back(ScriptItem::esc());
    return out;
}

}  // namespace

TEST_CASE("editing commands survive zero-length, huge, tiny and deeply nested geometry") {
    cmd::Registry reg;
    registerAllCommands(reg);
    const char* const kCommands[] = {
        "OFFSET",   "TRIM",  "EXTEND",    "FILLET",   "CHAMFER", "HATCH",  "BOUNDARY", "REGION",
        "JOIN",     "BREAK", "EXPLODE",   "STRETCH",  "SCALE",   "ROTATE", "MIRROR",   "ARRAY",
        "LENGTHEN", "PEDIT", "SPLINEDIT", "OVERKILL", "ALIGN",   "MOVE",   "COPY"};
    auto dir = std::filesystem::temp_directory_path() / "tbcad_odd_geometry";
    std::filesystem::create_directories(dir);
    std::mt19937 rng(20261025);
    int ran = 0;
    for (const char* name : kCommands) {
        if (!reg.find(name))
            continue;
        const std::string cmdName = name;
        INFO("command " << cmdName);
        for (int round = 0; round < 6; ++round) {
            Document doc;
            const std::vector<EntityId> ids = buildOddDrawing(doc);
            REQUIRE(ids.size() > 20);
            cmd::ScriptHost host(doc, reg);
            const std::string before = canonicalDump(doc);
            const auto t0 = std::chrono::steady_clock::now();
            (void)host.run(name, script(rng, ids, round));
            const auto secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            CHECK_MESSAGE(secs < kSlowest,
                          cmdName << " round " << round << " took " << secs << " s");
            const auto file = dir / (std::string(name) + ".tbcad");
            REQUIRE(io::writeTbcad(doc, file).ok());
            io::IoReport report;
            auto back = io::readTbcad(file, report);
            REQUIRE(back.ok());
            CHECK(canonicalDump(*back.value()) == canonicalDump(doc));
            while (host.undo()) {
            }
            CHECK(canonicalDump(doc) == before);
            ++ran;
        }
    }
    CHECK(ran > 80);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
