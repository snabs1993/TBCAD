// Whole-drawing edits on a drawing that looks like a real site plan (texts in an SHX style, MTEXT,
// circles, blocks with attributes, solid hatches sent to back) against the same edits on plain
// polylines and lines of the same size. A command that costs much more on the real mix has a
// path that depends on what is in the drawing (draw-order overrides, text layout, blocks ...):
// this is how the ERASE ALL and open-time regressions showed up. Sizes: TBCAD_BENCH_N (pairs).
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

    explicit Site(bool mixed) {
        dir = std::filesystem::temp_directory_path() /
              ("tbcad_edit_family_perf_" + std::to_string(::getpid()) + (mixed ? "m" : "p"));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_BENCH_N");
        n = env ? std::atoi(env) : 20000;
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

    Timing bench(const std::vector<std::string>& lines) {
        settle();
        core->selectAll();
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
};

}  // namespace

TEST_CASE("perf: editing commands cost about the same on a real mix as on plain geometry") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // MATCHPROP takes its destinations one pick at a time: 200 picks along the first row.
    std::vector<std::string> matchLines = {"MATCHPROP", "@click0,0"};
    for (int i = 1; i <= 200; ++i)
        matchLines.push_back("@click" + std::to_string(i * 30) + ",0");
    matchLines.push_back("");
    const std::vector<Case> cases = {
        {"COPY", {"COPY", "0,0", "5,5", ""}},
        {"MIRROR", {"MIRROR", "0,0", "10,0", "N"}},
        {"ARRAYRECT", {"ARRAYRECT", ""}},
        {"EXPLODE", {"EXPLODE"}},
        {"CHPROP-C", {"CHPROP", "Color", "1"}},
        {"CHPROP-L", {"CHPROP", "Layer", "0"}},
        {"LAYER-OFF", {"-LAYER", "Off", "0", ""}},
        {"LAYER-LOCK", {"-LAYER", "Lock", "0", ""}},
        {"MATCHPROP", matchLines},
        {"MOVE", {"MOVE", "0,0", "5,5"}},
        {"ROTATE", {"ROTATE", "0,0", "30"}},
        {"SCALE", {"SCALE", "0,0", "1.5"}},
        {"STRETCH", {"STRETCH", "-100000,-100000", "100000,100000", "0,0", "5,0"}},
        {"ERASE", {"ERASE"}},
        {"PURGE", {"PURGE", "", "Yes"}},
        {"AUDIT", {"AUDIT", "Yes"}},
    };
    // TBCAD_BENCH_ONLY=LABEL runs one command on the real mix only (to attach a profiler).
    if (const char* only = std::getenv("TBCAD_BENCH_ONLY")) {
        Site s(true);
        for (const Case& c : cases)
            if (std::string(only) == c.label)
                for (int k = 0; k < 4; ++k) {
                    const Timing t = s.bench(c.lines);
                    std::printf("%s run %.0f undo %.0f redo %.0f\n", c.label, t.runMs, t.undoMs,
                                t.redoMs);
                }
        return;
    }
    std::vector<Timing> plain, mixed;
    {
        Site s(false);
        std::printf("plain: %zu objects\n", s.core->document().modelSpaceCount());
        for (const Case& c : cases)
            plain.push_back(s.bench(c.lines));
    }
    {
        Site s(true);
        std::printf("mixed: %zu objects\n", s.core->document().modelSpaceCount());
        for (const Case& c : cases)
            mixed.push_back(s.bench(c.lines));
    }
    std::printf("%-10s %22s %22s %22s\n", "", "run plain/mixed", "undo plain/mixed",
                "redo plain/mixed");
    for (std::size_t i = 0; i < cases.size(); ++i)
        std::printf("%-10s %10.0f /%9.0f %10.0f /%9.0f %10.0f /%9.0f\n", cases[i].label,
                    plain[i].runMs, mixed[i].runMs, plain[i].undoMs, mixed[i].undoMs,
                    plain[i].redoMs, mixed[i].redoMs);
    // The mix has about 40% more objects and costlier ones: 3x plus slack is a path that depends
    // on the content.
    for (std::size_t i = 0; i < cases.size(); ++i) {
        INFO(cases[i].label);
        CHECK(mixed[i].runMs < 3 * plain[i].runMs + 300);
        CHECK(mixed[i].undoMs < 3 * plain[i].undoMs + 300);
        CHECK(mixed[i].redoMs < 3 * plain[i].redoMs + 300);
    }
}
