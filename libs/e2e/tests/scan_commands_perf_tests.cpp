// Commands that read every object of a big drawing (QSELECT, SELECTSIMILAR, LIST, CHANGE, the
// layer tools, GROUP, DRAWORDER, text tools, BLOCK, COPYCLIP, FIND) on a real mix of about 392k
// objects. Each is timed with the whole drawing selected; one that takes more than 3 s in an
// optimised build loops badly (the FILTER intersection once took 69 s, QSELECT Append 25 s). The
// slowest honest ones, CHANGE and LAYCHANGE, touch every object and take about a second. Sizes:
// TBCAD_BENCH_N pairs; 170000 is the 392k drawing, the default 40000 keeps the debug and
// sanitizer CI runs short.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Timing {
    double runMs = 0, undoMs = 0, redoMs = 0;
};

struct Site {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    int n = 0;

    explicit Site(int pairs = 0) {
        const bool mixed = true;
        dir = std::filesystem::temp_directory_path() /
              ("tbcad_scan_perf_" + std::to_string(::getpid()) + "scan");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_BENCH_N");
        n = pairs > 0 ? pairs : env ? std::atoi(env) : 40000;
        auto tx = core->document().begin("gen");
        TextStyleId shx{};
        BlockId block{};
        LayerId other{};
        if (mixed) {
            TextStyle ts;
            ts.name = "ROMANS";
            ts.shxFile = "romans.shx";
            ts.fontFamily = "Sarabun";
            shx = tx.addTextStyle(ts).value();
            other = tx.addLayer({.name = "Topo"}).value();
            block = tx.addBlock({.name = "TREE"}).value();
            (void)tx.add(Entity{{}, CircleData{{0, 0}, 2}}, block);
            TextData d;
            d.position = {3, 0};
            d.height = 1.5;
            d.utf8 = "TAG";
            d.attdef = AttDefInfo{.tag = "NAME", .prompt = "Name", .defaultValue = "x"};
            Entity a;
            a.data = std::move(d);
            (void)tx.add(std::move(a), block);
        }
        // Something for PURGE to take away.
        (void)tx.addLayer({.name = "UNUSED"});
        (void)tx.addBlock({.name = "UNUSED"});
        {
            TextStyle us;
            us.name = "UNUSED";
            (void)tx.addTextStyle(us);
        }
        std::vector<EntityId> toBack;
        for (int i = 0; i < n; ++i) {
            const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
            PolylineData p;
            for (int k = 0; k < 8; ++k)
                p.v.push_back({{x + k, y + k % 3}, 0, 0, 0});
            Entity e;
            e.data = std::move(p);
            if (i == 0)
                e.common.color = Color::index(3);  // the source of MATCHPROP
            (void)tx.add(std::move(e));
            (void)tx.add(Entity{{}, LineData{{x, y}, {x + 20, y + 10}}});
            if (!mixed)
                continue;
            if (i % 10 == 0) {
                TextData t;
                t.position = {x, y + 12};
                t.alignPoint = t.position;
                t.height = 2.5;
                t.utf8 = "EL " + std::to_string(100 + i % 900) + ".25";
                t.style = shx;
                Entity te;
                te.data = std::move(t);
                te.common.layer = other;
                (void)tx.add(std::move(te));
                MTextData m;
                m.position = {x, y + 20};
                m.height = 2;
                m.width = 25;
                m.style = shx;
                m.utf8 = "Boundary of lot " + std::to_string(i);
                (void)tx.add(Entity{{}, std::move(m)});
                (void)tx.add(Entity{{}, CircleData{{x + 5, y + 5}, 1.5}});
            }
            if (i % 40 == 0) {
                InsertData in;
                in.block = block;
                in.position = {x + 10, y + 3};
                in.attribs.push_back({"NAME", "T" + std::to_string(i)});
                (void)tx.add(Entity{{}, std::move(in)});
            }
            if (i % 200 == 0) {
                HatchPolylineEdge loop;
                loop.v = {{{x, y}, 0, 0, 0},
                          {{x + 20, y}, 0, 0, 0},
                          {{x + 20, y + 10}, 0, 0, 0},
                          {{x, y + 10}, 0, 0, 0}};
                HatchData h;
                h.loops.push_back({{HatchEdge{loop}}, true});
                auto id = tx.add(Entity{{}, std::move(h)});
                if (id)
                    toBack.push_back(id.value());
            }
        }
        for (EntityId id : toBack)
            (void)tx.sendToBack(id);
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // Closed rooms (a rectangle, a round island and a few loose lines) in rows below the drawing.
    void addRooms(int count) {
        auto tx = core->document().begin("rooms");
        for (int i = 0; i < count; ++i) {
            const double x = (i % 500) * 30.0, y = -1000.0 - (i / 500) * 30.0;
            PolylineData r;
            r.closed = true;
            r.v = {{{x, y}, 0, 0, 0}, {{x + 20, y}, 0, 0, 0}, {{x + 20, y + 20}, 0, 0, 0},
                   {{x, y + 20}, 0, 0, 0}};
            Entity e;
            e.data = std::move(r);
            (void)tx.add(std::move(e));
            (void)tx.add(Entity{{}, CircleData{{x + 10, y + 10}, 3}});
            (void)tx.add(Entity{{}, LineData{{x + 2, y + 2}, {x + 6, y + 3}}});
            (void)tx.add(Entity{{}, LineData{{x + 14, y + 15}, {x + 18, y + 17}}});
        }
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // One big room (a rectangle) with `islands` round islands and as many stray lines inside, at
    // x = -100000.
    void addIslandRoom(int islands) {
        auto tx = core->document().begin("island room");
        const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(islands))));
        const double w = side * 20.0 + 20;
        PolylineData r;
        r.closed = true;
        r.v = {{{-100000, 0}, 0, 0, 0},
               {{-100000 + w, 0}, 0, 0, 0},
               {{-100000 + w, w}, 0, 0, 0},
               {{-100000, w}, 0, 0, 0}};
        Entity e;
        e.data = std::move(r);
        (void)tx.add(std::move(e));
        for (int i = 0; i < islands; ++i) {
            const double x = -100000 + 20 + (i % side) * 20.0, y = 20 + (i / side) * 20.0;
            (void)tx.add(Entity{{}, CircleData{{x, y}, 4}});
            (void)tx.add(Entity{{}, LineData{{x + 8, y + 8}, {x + 14, y + 12}}});
        }
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // One polyline of `count` vertices (a contour) at y = -300000, and a line across its middle.
    void addLongPolyline(int count) {
        auto tx = core->document().begin("contour");
        PolylineData p;
        for (int i = 0; i < count; ++i)
            p.v.push_back({{i * 2.0, -300000 + (i % 2) * 1.5 + std::sin(i * 0.01) * 50}, 0, 0, 0});
        Entity e;
        e.data = std::move(p);
        (void)tx.add(std::move(e));
        (void)tx.add(Entity{{}, LineData{{count * 1.0, -300100}, {count * 1.0, -299900}}});
        // A second contour with another phase: the two cross about every 300 vertices.
        PolylineData q;
        for (int i = 0; i < count; ++i)
            q.v.push_back({{i * 2.0, -300000 + 3 + std::sin(i * 0.01 + 1) * 50}, 0, 0, 0});
        Entity e2;
        e2.data = std::move(q);
        (void)tx.add(std::move(e2));
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // A row of short lines at y = -500, each with a boundary a few units away on both sides.
    void addExtendRow(int count) {
        auto tx = core->document().begin("extend row");
        for (int i = 0; i < count; ++i) {
            const double x = i * 30.0;
            (void)tx.add(Entity{{}, LineData{{x, -500}, {x + 10, -500}}});
            (void)tx.add(Entity{{}, LineData{{x - 5, -510}, {x - 5, -490}}});
            (void)tx.add(Entity{{}, LineData{{x + 15, -510}, {x + 15, -490}}});
        }
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // Frames until nothing is left waiting to be built, so one command does not pay for the last.
    void settle() {
        for (int i = 0; i < 400; ++i) {
            const auto t = std::chrono::steady_clock::now();
            core->updateFrame();
            if (ms(t) < 3)
                break;
        }
    }

    Timing bench(const std::vector<std::string>& lines, bool preselect = true) {
        settle();
        if (preselect)
            core->selectAll();
        else
            core->clearSelection();
        const std::size_t depth = core->undoStack().undoCount();
        auto t = std::chrono::steady_clock::now();
        for (const std::string& l : lines) {
            // "@click0,0": a pick at a world point; "@window": a window around the whole drawing.
            if (l.rfind("@click", 0) == 0) {
                double x = 0, y = 0;
                std::sscanf(l.c_str(), "@click%lf,%lf", &x, &y);
                core->updateFrame();
                core->click({core->camera().worldToScreen({x, y}), false, false});
            } else if (l.rfind("@view", 0) == 0) {
                // "@view x0,y0,x1,y1": the view frames this box (HATCH only looks at the view).
                double a = 0, b = 0, c = 0, d = 0;
                std::sscanf(l.c_str(), "@view%lf,%lf,%lf,%lf", &a, &b, &c, &d);
                core->zoomToBox(BBox2{{a, b}, {c, d}});
                core->updateFrame();
            } else if (l == "@window") {
                core->updateFrame();
                core->click({{2, 2}, false, false});
                core->click({{1398, 898}, false, false});
            } else {
                core->submitText(l);
            }
        }
        core->updateFrame();
        Timing r;
        r.runMs = ms(t);
        if (!core->runner().active() || core->promptText().rfind("Command", 0) == 0) {
        } else {
            core->submitText("");
        }
        // A command that changed nothing leaves nothing to undo: do not undo the drawing itself.
        const bool changed = core->undoStack().undoCount() > depth;
        t = std::chrono::steady_clock::now();
        const bool undone = changed && core->undo();
        core->updateFrame();
        r.undoMs = ms(t);
        t = std::chrono::steady_clock::now();
        const bool redone = changed && core->redo();
        core->updateFrame();
        r.redoMs = ms(t);
        if (undone)
            core->undo();
        core->updateFrame();
        (void)redone;
        return r;
    }
};

struct Case {
    const char* label;
    std::vector<std::string> lines;
    double budgetMs = 3000;  // joining 170k pairs builds a polyline each
};

}  // namespace

TEST_CASE("perf: commands that scan the whole drawing stay fast on 392k objects") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<Case> cases = {
        {"QSELECT-type", {"QSELECT", "Type", "LINE", "Equals", "Replace"}},
        {"QSELECT-not", {"QSELECT", "Layer", "Topo", "Not", "Replace"}},
        {"QSELECT-append", {"QSELECT", "Layer", "Topo", "Equals", "Append"}},
        {"QSELECT-radius", {"QSELECT", "Radius", "1.5", "Greater", "Replace"}},
        {"SELECTSIMILAR", {"SELECTSIMILAR"}},
        {"LIST", {"LIST"}},
        {"CHANGE", {"CHANGE", "0,0"}},
        {"LAYCHANGE", {"LAYCHANGE", "Topo"}},
        {"LAYISO", {"LAYISO"}},
        {"LAYWALK", {"-LAYWALK", "Show", "Topo"}},
        {"BLOCKCOUNT", {"BLOCKCOUNT"}},
        {"GROUP", {"GROUP", "Name", "ALLOFIT"}},
        {"DRAWORDER", {"DRAWORDER", "Back"}},
        {"JUSTIFYTEXT", {"JUSTIFYTEXT", "Center"}},
        {"SCALETEXT", {"SCALETEXT", "Existing", "2"}},
        {"TXT2MTXT", {"TXT2MTXT"}},
        {"FINDSTRAY", {"FINDSTRAY"}},
        {"COPYCLIP", {"COPYCLIP"}},
        {"BLOCK", {"BLOCK", "ALLOFIT", "0,0"}},
        {"ZOOM-E", {"ZOOM", "E"}},
        {"FIND-count", {"FIND", "EL ", "", "No", "No"}},
        {"FIND-replace", {"FIND", "EL ", "ELEV ", "No", "No"}},
        {"PROP-layer", {"-PROP", "Layer", "Topo"}},
        {"PROP-color", {"-PROP", "Only", "Line", "Color", "3"}},
        {"PROP-set", {"-PROP", "Only", "Circle", "Set", "radius", "2"}},
        {"PROP-delete", {"-PROP", "Only", "Line", "Delete"}},
        {"JOIN", {"JOIN"}, 8000},
        {"PEDIT-join", {"PEDIT", "Multiple", "Yes", "Join", "0.001", ""}, 8000},
        {"OVERKILL", {"OVERKILL", "ALL", "", ""}, 8000},
        {"REVERSE", {"REVERSE", "ALL", ""}},
        {"SETBYLAYER", {"SETBYLAYER", "ALL", "", "Yes", "Yes"}},
        {"ISOLATE", {"ISOLATE", "ALL", ""}},
        {"SELECTALL", {"SELECTALL"}},
        {"FIND-nomatch", {"FIND", "zzzqq", "yy", "Yes", "No"}},
    };
    Site s;
    std::printf("%zu objects\n", s.core->document().modelSpaceCount());
    std::printf("%-16s %8s %8s %8s\n", "", "run", "undo", "redo");
    for (const Case& c : cases) {
        if (const char* only = std::getenv("TBCAD_BENCH_ONLY");
            only && std::string(only) != c.label)
            continue;
        const Timing t = s.bench(c.lines);
        std::printf("%-16s %8.0f %8.0f %8.0f   %s\n", c.label, t.runMs, t.undoMs, t.redoMs,
                    s.core->history().empty() ? "" : s.core->history().back().c_str());
#ifdef NDEBUG
        INFO(c.label);
        CHECK(t.runMs < c.budgetMs);
#endif
    }
}

// Tables with thousands of rows: layers, blocks and their inserts, as in a drawing that was
// assembled from many files.
TEST_CASE("perf: table commands with thousands of layers and blocks stay fast") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Site s(20000);
    constexpr int kLayers = 3000, kBlocks = 3000, kInserts = 30000;
    {
        auto tx = s.core->document().begin("tables");
        std::vector<LayerId> layers;
        for (int i = 0; i < kLayers; ++i)
            layers.push_back(tx.addLayer({.name = "LAYER" + std::to_string(i)}).value());
        std::vector<BlockId> blocks;
        for (int b = 0; b < kBlocks; ++b) {
            const BlockId id = tx.addBlock({.name = "BLK" + std::to_string(b)}).value();
            for (int k = 0; k < 3; ++k)
                (void)tx.add(Entity{{}, LineData{{k * 2.0, 0}, {k * 2.0 + 1, 4.0 + b % 5}}}, id);
            blocks.push_back(id);
        }
        for (int i = 0; i < kInserts; ++i) {
            InsertData in;
            in.block = blocks[static_cast<std::size_t>(i % kBlocks)];
            in.position = {(i % 300) * 40.0, 20000.0 + (i / 300) * 40.0};
            Entity e;
            e.data = Indirect<InsertData>(std::move(in));
            e.common.layer = layers[static_cast<std::size_t>(i % kLayers)];
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    s.core->zoomExtents();
    s.core->updateFrame();
    const std::vector<Case> cases = {
        {"LAYER-OFF", {"-LAYER", "Off", "*", ""}},
        {"LAYER-FREEZE", {"-LAYER", "Freeze", "*", ""}},
        {"LAYER-LOCK", {"-LAYER", "Lock", "*", ""}},
        {"LAYISO", {"LAYISO"}},
        {"PURGE", {"PURGE", "", "Yes"}},
        {"BLOCKCOUNT", {"BLOCKCOUNT"}},
        {"EXPLODE", {"EXPLODE"}},
        {"QSELECT-layer", {"QSELECT", "Layer", "LAYER7", "Equals", "Replace"}},
        {"SELECTSIMILAR", {"SELECTSIMILAR"}},
        {"AUDIT", {"AUDIT"}},
    };
    std::printf("%zu objects, %d layers, %d blocks\n", s.core->document().modelSpaceCount(),
                kLayers, kBlocks);
    for (const Case& c : cases) {
        const Timing t = s.bench(c.lines);
        std::printf("%-16s %8.0f %8.0f %8.0f   %s\n", c.label, t.runMs, t.undoMs, t.redoMs,
                    s.core->history().empty() ? "" : s.core->history().back().c_str());
#ifdef NDEBUG
        INFO(c.label);
        CHECK(t.runMs < c.budgetMs);
#endif
    }
}

// One object at a time on the big drawing: what an operator does all day. Each step must cost
// about what it costs on a small drawing, not grow with the 392k objects around it.
TEST_CASE("perf: single-object edits on a drawing of 392k objects stay interactive") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Site s;
    // The first cell: a polyline from (0,0) rising to (7,1) and a line (0,0)-(20,10).
    const std::vector<Case> cases = {
        {"ERASE-one", {"ERASE", "@click10,5", ""}},
        {"MOVE-one", {"MOVE", "@click10,5", "", "0,0", "50,50"}},
        {"COPY-one", {"COPY", "@click10,5", "", "0,0", "50,50", ""}},
        {"ROTATE-one", {"ROTATE", "@click10,5", "", "0,0", "30"}},
        {"SCALE-one", {"SCALE", "@click10,5", "", "0,0", "2"}},
        {"MIRROR-one", {"MIRROR", "@click10,5", "", "0,0", "10,0", "N"}},
        {"STRETCH-one", {"STRETCH", "5,3", "12,8", "0,0", "5,0"}},
        {"ARRAY-one", {"ARRAYRECT", "@click10,5", "", ""}},
        {"OFFSET-one", {"OFFSET", "3", "@click10,5", "@click12,2", ""}},
        {"TRIM-one", {"TRIM", "", "@click10,5", ""}},
        {"BREAK-one", {"BREAK", "@click10,5", "@click14,7"}},
        {"CHPROP-one", {"CHPROP", "@click10,5", "", "Color", "1"}},
        {"LIST-one", {"LIST", "@click10,5", ""}},
    };
    std::printf("%zu objects\n", s.core->document().modelSpaceCount());
    for (const Case& c : cases) {
        const Timing t = s.bench(c.lines, false);
        std::printf("%-16s %8.0f %8.0f %8.0f   %s\n", c.label, t.runMs, t.undoMs, t.redoMs,
                    s.core->history().empty() ? "" : s.core->history().back().c_str());
#ifdef NDEBUG
        INFO(c.label);
        CHECK(t.runMs < 100);  // a pick costs what it costs on a small drawing (about 20 ms)
#endif
    }
}

// TRIM and EXTEND by fence and crossing, OFFSET, FILLET and HATCH pick points on the big drawing:
// each looks for its neighbours through the spatial index, so the cost follows the objects it
// touches, not the 392k around them.
TEST_CASE("perf: trim, extend, offset, fillet and hatch on a big drawing stay fast") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Site s;
    s.addRooms(s.n / 2);
    s.addExtendRow(std::min(s.n, 500));
    s.addIslandRoom(std::min(s.n / 4, 5000));
    const std::vector<Case> cases = {
        {"TRIM-fence-row", {"TRIM", "", "Fence", "0,1", "15000,1", "", ""}},
        {"TRIM-fence-diag", {"TRIM", "", "Fence", "0,0", "15000,3000", "", ""}},
        {"TRIM-crossing", {"TRIM", "", "Crossing", "0,0", "3000,300", ""}},
        {"TRIM-crossing-edges", {"TRIM", "@window", "", "Crossing", "0,0", "3000,300", ""}},
        // Every short line of the row has a boundary a few units ahead of its nearer end.
        {"EXTEND-crossing-row", {"EXTEND", "", "Crossing", "0,-505", "15000,-495", ""}},
        // Implied edge extension has to look at every object once (an edge can run on from afar).
        {"TRIM-edge-extend", {"TRIM", "", "Edge", "Extend", "@click10,5", ""}, 5000},
        {"OFFSET-polyline", {"OFFSET", "3", "@click3,1", "5,5", ""}},
        {"FILLET-polyline", {"FILLET", "R", "0.3", "Polyline", "@click1,1"}},
        {"HATCH-room", {"@view -50,-1100,800,-900", "HATCH", "5,-995", ""}},
        {"HATCH-room-far", {"@view 14000,-1100,15000,-900", "HATCH", "14505,-1025", ""}},
        {"HATCH-islands", {"@view -100100,-100,-90000,10000", "HATCH", "-99995,5", ""}},
        {"HATCH-no-boundary", {"@view 0,0,300,100", "HATCH", "10,5", ""}},
    };
    std::printf("%zu objects\n", s.core->document().modelSpaceCount());
    for (const Case& c : cases) {
        const Timing t = s.bench(c.lines, false);
        std::printf("%-20s %8.0f %8.0f %8.0f   %s\n", c.label, t.runMs, t.undoMs, t.redoMs,
                    s.core->history().empty() ? "" : s.core->history().back().c_str());
#ifdef NDEBUG
        INFO(c.label);
        CHECK(t.runMs < c.budgetMs);
#endif
    }
}

// One polyline of many vertices (a contour line): the edit tools may walk it, but not once per
// vertex.
TEST_CASE("perf: edits of one polyline of 100k vertices stay quick") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Site s(100);
    const int count = std::getenv("TBCAD_BENCH_N") ? std::atoi(std::getenv("TBCAD_BENCH_N")) : 100000;
    s.addLongPolyline(count);
    const double mid = count * 1.0, y = -300000;
    const std::string midx = std::to_string(mid);
    const std::vector<Case> cases = {
        {"OFFSET-contour", {"OFFSET", "5", "@click0,-300000", "-1000,-299990", ""}, 8000},
        {"TRIM-contour", {"TRIM", "", "@click0,-300000", ""}},
        {"TRIM-by-contour", {"TRIM", "@click100,-299950", "", "@click100,-299976", ""}},
        {"EXTEND-contour", {"EXTEND", "", "@click0,-300000", ""}},
        {"BREAK-contour", {"BREAK", "@click0,-300000", "100,-300000"}},
        {"FILLET-contour", {"FILLET", "R", "0.2", "Polyline", "@click0,-300000"}},
        {"EXPLODE-contour", {"EXPLODE", "@click0,-300000", ""}},
        {"CHAMFER-contour", {"CHAMFER", "Distance", "0.2", "0.2", "Polyline", "@click0,-300000"}, 3000},
        {"REVERSE-contour", {"REVERSE", "@click0,-300000", ""}, 3000},
        {"SCALE-contour", {"SCALE", "@click0,-300000", "", "0,-300000", "2"}, 3000},
        {"ROTATE-contour", {"ROTATE", "@click0,-300000", "", "0,-300000", "30"}, 3000},
        {"MIRROR-contour", {"MIRROR", "@click0,-300000", "", "0,-300000", "100,-300000", "No"}, 3000},
        {"COPY-contour", {"COPY", "@click0,-300000", "", "0,0", "5,5"}, 3000},
        {"DIVIDE-contour", {"DIVIDE", "@click0,-300000", "30000"}, 3000},
        {"MEASURE-contour", {"MEASURE", "@click0,-300000", "40"}, 3000},
        {"PEDIT-width-contour", {"PEDIT", "@click0,-300000", "Width", "3", ""}, 3000},
        {"PEDIT-fit-contour", {"PEDIT", "@click0,-300000", "Ltype", "ON", ""}, 3000},
        {"LIST-contour", {"LIST", "@click0,-300000", ""}},
        {"MOVE-contour", {"MOVE", "@click0,-300000", "", "0,0", "5,5"}},
    };
    (void)y;
    for (const Case& c : cases) {
        if (const char* only = std::getenv("TBCAD_BENCH_ONLY"); only && std::string(only) != c.label)
            continue;
        const Timing t = s.bench(c.lines, false);
        std::printf("%-20s %8.0f %8.0f %8.0f   %s\n", c.label, t.runMs, t.undoMs, t.redoMs,
                    s.core->history().empty() ? "" : s.core->history().back().c_str());
#ifdef NDEBUG
        INFO(c.label);
        CHECK(t.runMs < c.budgetMs);
#endif
    }
}

// What a hand does all day on a drawing of a few ten thousand objects: pick, hover, window select,
// drag a grip, switch a layer, zoom. Each step includes the frame after it.
TEST_CASE("perf: everyday interactions on a drawing of 100k objects") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef NDEBUG
    Site s(std::getenv("TBCAD_BENCH_N") ? 0 : 50000);
#else
    Site s(5000);  // (the debug and sanitizer runs stay short)
#endif
    ui::AppCore& c = *s.core;
    ui::UiRegistry uiRegistry;  // (grips, snaps and the panels live in the UI registry)
    registerAllUi(uiRegistry);
    c.registerBuiltinUi(uiRegistry);
    c.applyUiKeybindings(uiRegistry);
    std::printf("%zu objects\n", c.document().modelSpaceCount());
    auto step = [&](const char* what, auto&& fn) {
        const auto t = std::chrono::steady_clock::now();
        fn();
        c.updateFrame();
        const double took = ms(t);
        std::printf("  %-34s %8.1f ms   (%zu selected, %zu objects, prompt: %.30s)\n", what, took,
                    c.selection().size(), c.document().modelSpaceCount(), c.promptText().c_str());
        return took;
    };
    {
        const auto t = std::chrono::steady_clock::now();
        int frames = 0;
        while (c.sceneBuilding() && frames < 100000) {
            c.updateFrame();
            ++frames;
        }
        std::printf("  settle after build: %d frames, %.0f ms\n", frames, ms(t));
    }
    c.submitText("ZOOM");
    c.submitText("Window");
    c.submitText("-5,-5");
    c.submitText("205,105");
    c.updateFrame();
    CHECK(c.runner().active() == false);
    ui::PointerState on;
    on.screen = c.camera().worldToScreen({10, 5});
    step("zoom window to a cell", [] {});
    [[maybe_unused]] const double pickMs = step("pick one object", [&] { c.click(on); });
    {
        std::vector<double> idle;
        for (int i = 0; i < 10; ++i) {
            const auto t = std::chrono::steady_clock::now();
            c.updateFrame();
            idle.push_back(ms(t));
        }
        std::printf("   idle frames:");
        for (double v : idle)
            std::printf(" %.1f", v);
        std::printf("\n");
        std::sort(idle.begin(), idle.end());
        std::printf("  %-34s median %.2f ms, worst %.2f ms\n", "idle frame (nothing changed)",
                    idle[5], idle.back());
    }
    std::vector<double> hover, movedMs;
    for (int i = 0; i < 40; ++i) {
        ui::PointerState p;
        p.screen = {200.0 + i * 11, 300.0 + (i % 5) * 7};
        const auto t = std::chrono::steady_clock::now();
        c.pointerMoved(p);
        movedMs.push_back(ms(t));
        c.updateFrame();
        hover.push_back(ms(t));
    }
    std::sort(movedMs.begin(), movedMs.end());
    std::printf("  %-34s median %.2f ms\n", "  (of which pointerMoved)", movedMs[20]);
    std::sort(hover.begin(), hover.end());
    std::printf("  %-34s median %.2f ms, worst %.2f ms\n", "hover 40 positions", hover[20],
                hover.back());
#ifdef NDEBUG
    CHECK(pickMs < 100);
    CHECK(hover[20] < 20);
#endif
    step("window select", [&] {
        c.dragSelect({100, 100}, {700, 500}, {});
    });
    step("clear selection", [&] { c.clearSelection(); });
    // A grip drag: select a polyline, press on its first grip, move, release.
    c.click(on);
    c.updateFrame();
    const Vec2 grip = c.camera().worldToScreen({0, 0});
    step("press on a grip", [&] {
        ui::PointerState p;
        p.screen = grip;
        c.pointerMoved(p);
        std::printf("    pressOnGrip -> %d\n", c.pressOnGrip(p) ? 1 : 0);
    });
    step("drag the grip", [&] {
        ui::PointerState p;
        p.screen = {grip.x + 60, grip.y + 40};
        c.pointerMoved(p);
    });
    step("release the grip", [&] {
        ui::PointerState p;
        p.screen = {grip.x + 60, grip.y + 40};
        c.click(p);
    });
    c.pressEscape();
    step("undo the grip edit", [&] { (void)c.undo(); });
    c.pressEscape();
    [[maybe_unused]] const double extentsMs = step("zoom extents", [&] { c.zoomExtents(); });
#ifdef NDEBUG
    CHECK(extentsMs < 500);
#endif
    step("layer off (Topo, one tenth)", [&] {
        c.submitText("-LAYER");
        c.submitText("Off");
        c.submitText("Topo");
        c.submitText("");
    });
    step("layer on (Topo, one tenth)", [&] {
        c.submitText("-LAYER");
        c.submitText("On");
        c.submitText("Topo");
        c.submitText("");
    });
    while (c.sceneBuilding())
        c.updateFrame();
    std::vector<double> zoom;
    for (int i = 0; i < 30; ++i) {
        const auto t = std::chrono::steady_clock::now();
        c.zoomAtScreen({700, 450}, i % 2 ? -1.0 : 1.0);
        c.updateFrame();
        zoom.push_back(ms(t));
    }
    std::sort(zoom.begin(), zoom.end());
    std::printf("  %-34s median %.2f ms, worst %.2f ms\n", "30 wheel zoom steps", zoom[15],
                zoom.back());
    step("select all", [&] { c.selectAll(); });
    step("erase all", [&] {
        c.submitText("ERASE");
        c.pressEscape();
    });
    step("undo erase all", [&] { (void)c.undo(); });
    while (c.sceneBuilding())
        c.updateFrame();
}

// Saving and opening the native file on a mixed drawing: exact, and fast enough to never be the
// reason someone waits.
TEST_CASE("perf: .tbcad save and load of a mixed drawing is exact and quick") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Site s;
    const Document& doc = s.core->document();
    const auto file = s.dir / "mixed.tbcad";
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(io::writeTbcad(doc, file).ok());
    const double saveMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    io::IoReport report;
    t0 = std::chrono::steady_clock::now();
    auto back = io::readTbcad(file, report);
    const double loadMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(back.ok());
    std::printf("%zu objects: save %.0f ms, load %.0f ms, file %.1f MB\n", doc.modelSpaceCount(),
                saveMs, loadMs, std::filesystem::file_size(file) / 1048576.0);
    CHECK(back.value()->modelSpaceCount() == doc.modelSpaceCount());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));
#ifdef NDEBUG
    CHECK(saveMs < 8000);
    CHECK(loadMs < 8000);
#endif
}
