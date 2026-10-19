// Commands that read every object of a big drawing (QSELECT, SELECTSIMILAR, LIST, CHANGE, the
// layer tools, GROUP, DRAWORDER, text tools, BLOCK, COPYCLIP, FIND) on a real mix of about 392k
// objects. Each is timed with the whole drawing selected; one that takes more than 3 s in an
// optimised build loops badly (the FILTER intersection once took 69 s, QSELECT Append 25 s). The
// slowest honest ones, CHANGE and LAYCHANGE, touch every object and take about a second. Sizes:
// TBCAD_BENCH_N (pairs).
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"

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
        n = pairs > 0 ? pairs : env ? std::atoi(env) : 170000;
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
        {"FIND-count", {"FIND", "EL ", "", "No"}},
        {"FIND-replace", {"FIND", "EL ", "ELEV ", "No"}},
        {"PROP-layer", {"-PROP", "Layer", "Topo"}},
        {"PROP-color", {"-PROP", "Only", "Line", "Color", "3"}},
        {"PROP-set", {"-PROP", "Only", "Circle", "Set", "radius", "2"}},
        {"PROP-delete", {"-PROP", "Only", "Line", "Delete"}},
        {"JOIN", {"JOIN"}, 8000},
        {"PEDIT-join", {"PEDIT", "Multiple", "Yes", "Join", "0.001", ""}, 8000},
        {"FIND-nomatch", {"FIND", "zzzqq", "yy", "Yes"}},
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
        CHECK(t.runMs < 500);
#endif
    }
}
