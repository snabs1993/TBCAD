// The M1 gate: 1 000 000 random lines, zoom, pan, pick, window/crossing selection, save, reopen.
// Prints one line per measurement and PASS/FAIL against the milestone limits.
#include <GLFW/glfw3.h>
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "app.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/entity.h"
#include "tbcad/platform/paths.h"

namespace tbcad::app {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Report {
    std::ostringstream out;
    bool allPassed = true;

    void line(const std::string& name, double value, const char* unit, const std::string& limit,
              bool pass) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%-34s %12.2f %-4s  limit %-14s %s\n", name.c_str(), value,
                      unit, limit.c_str(), pass ? "PASS" : "FAIL");
        out << buf;
        allPassed = allPassed && pass;
    }
    void info(const std::string& text) { out << text << "\n"; }
};

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

}  // namespace

int App::runGate() {
    Report rep;
    ui::AppCore& core = *core_;
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        core.setViewportSize(std::max(1, w), std::max(1, h));
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    rep.info(std::string("TBCAD ") + TBCAD_VERSION + " gate on " + (renderer ? renderer : "?") +
             ", viewport " + std::to_string(core.camera().viewportW) + "x" +
             std::to_string(core.camera().viewportH));

    // 1. Generate.
    auto t0 = Clock::now();
    core.submitText("DEVGEN LINES " + std::to_string(options_.gateLines));
    double genMs = msSince(t0);
    const std::size_t count = core.document().modelSpaceCount();
    rep.line("generate " + std::to_string(options_.gateLines) + " lines", genMs, "ms", "info",
             count == static_cast<std::size_t>(options_.gateLines));
    std::fprintf(stderr, "gate: drawing generated in %.0f ms\n", genMs);
    rep.info("lines in drawing: " + std::to_string(count) +
             ", in index: " + std::to_string(core.spatialIndex().size()) +
             ", in scene: " + std::to_string(core.scene().entityCount()));

    // 2. Zoom extents, then draw.
    t0 = Clock::now();
    core.zoomExtents();
    core.updateFrame();  // like every frame of the app: re-tiles a drawing that outgrew its tiles
    renderDrawingOnce(true);
    double firstFrameMs = msSince(t0);
    rep.line("zoom extents + first frame", firstFrameMs, "ms", "info", true);

    std::vector<double> frames;
    for (int i = 0; i < 120; ++i) {
        auto f0 = Clock::now();
        renderDrawingOnce(true);
        frames.push_back(msSince(f0));
    }
    rep.line("static view, median frame", percentile(frames, 0.5), "ms", "info", true);
    {
        // Where a static frame goes: what was submitted, and how long the CPU spent on it
        // (the rest of the frame time is the GPU).
        const render::RenderStats& st = renderer_.stats();
        rep.info("static frame: " + std::to_string(st.drawCalls) + " draw calls, " +
                 std::to_string(st.vertices) + " vertices, " + std::to_string(st.tilesDrawn) +
                 " tiles, " + std::to_string(st.uploadedBytes) + " bytes uploaded, cpu " +
                 std::to_string(st.cpuMs) + " ms");
    }

    // 3. Pan and zoom about the cursor.
    frames.clear();
    const render::Camera2D start = core.camera();
    const Vec2 anchor{core.camera().viewportW * 0.5, core.camera().viewportH * 0.5};
    for (int i = 0; i < 240; ++i) {
        auto f0 = Clock::now();
        core.panPixels(7, 3);
        core.zoomAtScreen(anchor, (i / 60) % 2 == 0 ? 0.08 : -0.08);
        core.updateFrame();
        renderDrawingOnce(true);
        frames.push_back(msSince(f0));
    }
    double medianMs = percentile(frames, 0.5);
    double p95Ms = percentile(frames, 0.95);
    rep.line("pan/zoom, median fps", 1000.0 / medianMs, "fps", ">= 30 (60 goal)",
             1000.0 / medianMs >= 30);
    rep.line("pan/zoom, 5% worst frame fps", 1000.0 / p95Ms, "fps", ">= 20", 1000.0 / p95Ms >= 20);
    core.setCamera(start);

    // 4. Click selects the line under the cursor.
    {
        EntityId target = core.document().modelSpace()[count / 2];
        const Entity* e = core.document().find(target);
        const auto* line = entityAs<LineData>(*e);
        Vec2 mid = (line->a + line->b) * 0.5;
        BBox2 box;
        box.expand(line->a);
        box.expand(line->b);
        core.zoomToBox(box.inflated(200));
        core.clearSelection();
        t0 = Clock::now();
        core.click(ui::PointerState{core.camera().worldToScreen(mid), false, false});
        double clickMs = msSince(t0);
        const bool hit = std::find(core.selection().begin(), core.selection().end(), target) !=
                         core.selection().end();
        rep.line("click picks the line", clickMs, "ms", "< 50", hit && clickMs < 50);
        core.clearSelection();
        core.setCamera(start);
    }

    // 5. Window and crossing selection.
    {
        const render::Camera2D& cam = core.camera();
        const Vec2 a{cam.viewportW * 0.30, cam.viewportH * 0.30};
        const Vec2 b{cam.viewportW * 0.55, cam.viewportH * 0.60};
        t0 = Clock::now();
        core.dragSelect(a, b, ui::PointerState{b, false, false});
        double windowMs = msSince(t0);
        std::size_t inWindow = core.selection().size();
        rep.line("window select (" + std::to_string(inWindow) + " lines)", windowMs, "ms", "< 100",
                 inWindow > 1000 && windowMs < 100);
        core.clearSelection();
        t0 = Clock::now();
        core.dragSelect(b, a, ui::PointerState{a, false, false});
        double crossMs = msSince(t0);
        std::size_t crossing = core.selection().size();
        rep.line("crossing select (" + std::to_string(crossing) + " lines)", crossMs, "ms", "< 100",
                 crossing >= inWindow && crossMs < 100);
        // The selection must also draw.
        renderDrawingOnce(true);
        core.clearSelection();
    }

    // 6. Save and reopen.
    {
        std::filesystem::path file = std::filesystem::temp_directory_path() / "tbcad_gate.tbcad";
        const std::string before = canonicalDump(core.document());
        t0 = Clock::now();
        auto saved = core.saveAs(file);
        double saveMs = msSince(t0);
        rep.line("save", saveMs, "ms", "< 3000", saved.ok() && saveMs < 3000);
        core.closeDocument(core.activeDocument());  // otherwise the open drawing is just activated
        t0 = Clock::now();
        auto opened = core.openFile(file);
        double openMs = msSince(t0);
        const bool same = opened.ok() && canonicalDump(core.document()) == before;
        rep.line("reopen (includes building the view)", openMs, "ms", "< 3000",
                 opened.ok() && openMs < 3000);
        rep.line("reopened drawing is identical", same ? 1 : 0, "", "1", same);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        std::filesystem::remove(std::filesystem::path(file).concat(".bak"), ec);
    }

    rep.info(rep.allPassed ? "GATE PASSED" : "GATE FAILED");
    const std::string text = rep.out.str();
    std::fputs(text.c_str(), stdout);
    if (!options_.report.empty()) {
        std::ofstream out(options_.report);
        out << text;
    }
    core_->shutdown();
    return rep.allPassed ? 0 : 1;
}

// --gate-file: the same measurements on any drawing, with whole frames (UI included) so a slow
// panel or overlay shows up too. Nothing is asserted; it prints timings.
namespace {

std::size_t vertexCount(const DisplayList& list) {
    std::size_t n = 0;
    for (const DisplayItem& item : list.items)
        std::visit(
            [&](const auto& it) {
                using T = std::decay_t<decltype(it)>;
                if constexpr (std::is_same_v<T, DisplayFill>) {
                    for (const auto& ring : it.rings)
                        n += ring.size();
                } else {
                    n += it.pts.size();
                }
            },
            item);
    return n;
}

// The objects with the most geometry at the zoom-extents view, so a slow drawing shows what makes
// it slow (a hatch pattern drawn line by line, a block with a huge hatch inside).
void printHeaviest(const ui::AppCore& core) {
    const Document& doc = core.document();
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = core.camera().pixelSizeMm();
    struct Heavy {
        std::size_t vertices = 0;
        std::size_t items = 0;
        const Entity* e = nullptr;
    };
    std::vector<Heavy> all;
    std::size_t total = 0;
    std::map<int, std::size_t> byKind;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        DisplayList list;
        geom::buildDisplay(doc, *e, ctx, list);
        const std::size_t v = vertexCount(list);
        total += v;
        byKind[static_cast<int>(kind(*e))] += v;
        all.push_back({v, list.items.size(), e});
    }
    auto describe = [&](const Entity& e) {
        std::string s = "kind " + std::to_string(static_cast<int>(kind(e)));
        if (const auto* ins = entityAs<InsertData>(e)) {
            const BlockDef* b = doc.block(ins->block);
            s = "insert of " + (b ? b->name : std::string("?"));
        } else if (const auto* h = entityAs<HatchData>(e)) {
            s = "hatch " + h->pattern + (h->solid ? " (solid)" : "");
        }
        if (const Layer* l = doc.layer(e.common.layer))
            s += " on " + l->name;
        return s;
    };
    std::printf("geometry at zoom extents: %zu vertices (pixel %.4g)\n", total, ctx.pixelSizeMm);
    for (const auto& [k, v] : byKind)
        if (v * 50 >= total)
            std::printf("  kind %-3d %12zu vertices\n", k, v);
    const std::size_t top = std::min<std::size_t>(12, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(top), all.end(),
                      [](const Heavy& a, const Heavy& b) { return a.vertices > b.vertices; });
    for (std::size_t i = 0; i < top; ++i)
        std::printf("  %10zu vertices %8zu items  #%llu %s\n", all[i].vertices, all[i].items,
                    static_cast<unsigned long long>(all[i].e->common.id.value),
                    describe(*all[i].e).c_str());
}

}  // namespace

int App::runGateFile() {
    ui::AppCore& core = *core_;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    core.setViewportSize(std::max(1, w - 600), std::max(1, h - 200));

    auto t0 = Clock::now();
    auto opened = core.openFile(platform::pathFromUtf8(options_.gateFile));
    const double openMs = msSince(t0);
    if (!opened.ok()) {
        std::fprintf(stderr, "gate-file: cannot open %s\n", options_.gateFile.c_str());
        core.shutdown();
        return 1;
    }
    std::printf("file: %s\n", options_.gateFile.c_str());
    std::printf("open: %.0f ms (%zu entities in model space)\n", openMs,
                core.document().modelSpaceCount());
    for (const auto& [name, ms] : core.openTimings())
        std::printf("  %-32s %8.0f ms\n", name.c_str(), ms);

    profileStart();
    double gpuWait = 0;
    int counted = 0;
    auto frames = [&](int n, const std::function<void(int)>& step) {
        std::vector<double> ms;
        gpuWait = 0;
        counted = n;
        profileReport(nullptr, 0, 1);  // drop what earlier frames added
        for (int i = 0; i < n; ++i) {
            auto f0 = Clock::now();
            step(i);
            frame();
            auto g0 = Clock::now();
            glFinish();
            gpuWait += msSince(g0);
            ms.push_back(msSince(f0));
        }
        return ms;
    };
    auto report = [&](const char* name, const std::vector<double>& ms) {
        const double med = percentile(ms, 0.5);
        std::printf("%-28s median %8.2f ms (%6.1f fps)  worst %8.2f ms\n", name, med,
                    med > 0 ? 1000.0 / med : 0.0, percentile(ms, 1.0));
        profileReport("ms/frame by phase:", gpuWait, counted);
        const render::RenderStats& st = renderer_.stats();
        std::printf("  last frame: %u draw calls, %zu vertices, %u tiles, cpu %.2f ms\n",
                    static_cast<unsigned>(st.drawCalls), static_cast<std::size_t>(st.vertices),
                    static_cast<unsigned>(st.tilesDrawn), st.cpuMs);
    };

    core.zoomExtents();
    t0 = Clock::now();
    frame();
    glFinish();
    std::printf("zoom extents + first frame: %.0f ms\n", msSince(t0));
    printHeaviest(core);
    {
        const std::vector<double> ms = frames(30, [](int) {});
        std::printf("static frame times (ms):");
        for (double v : ms)
            std::printf(" %.0f", v);
        std::printf("\n");
        report("static view", ms);
    }

    const render::Camera2D start = core.camera();
    const Vec2 anchor{core.camera().viewportW * 0.5, core.camera().viewportH * 0.5};
    report("pan/zoom", frames(120, [&](int i) {
               core.panPixels(7, 3);
               core.zoomAtScreen(anchor, (i / 30) % 2 == 0 ? 0.08 : -0.08);
           }));
    core.setCamera(start);

    t0 = Clock::now();
    core.selectAll();
    std::printf("select all: %.0f ms (%zu selected)\n", msSince(t0), core.selection().size());
    t0 = Clock::now();
    frame();
    glFinish();
    std::printf("first frame after select all: %.0f ms\n", msSince(t0));
    report("frames with selection", frames(30, [](int) {}));
    report("pan/zoom with selection", frames(60, [&](int i) {
               core.panPixels(7, 3);
               core.zoomAtScreen(anchor, (i / 30) % 2 == 0 ? 0.08 : -0.08);
           }));

    // Editing everything at once, and taking it back.
    for (const char* edit :
         {"MOVE;ALL;;0,0;100,0", "ROTATE;ALL;;0,0;30", "SCALE;ALL;;0,0;1.5", "ERASE;ALL;"}) {
        std::string cmds = edit;
        t0 = Clock::now();
        for (std::size_t at = 0; at <= cmds.size();) {
            const std::size_t end = cmds.find(';', at);
            core.submitText(cmds.substr(at, end == std::string::npos ? end : end - at));
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        frame();
        glFinish();
        const double doMs = msSince(t0);
        t0 = Clock::now();
        core.submitText("UNDO");
        frame();
        glFinish();
        const double undoMs = msSince(t0);
        t0 = Clock::now();
        core.submitText("REDO");
        frame();
        glFinish();
        std::printf("%-22s %8.0f ms, undo %6.0f ms, redo %6.0f ms (%zu objects after)\n", edit,
                    doMs, undoMs, msSince(t0), core.document().modelSpaceCount());
        core.submitText("UNDO");
        frame();
    }

    t0 = Clock::now();
    core.clearSelection();
    frame();
    glFinish();
    std::printf("clear selection + frame: %.0f ms\n", msSince(t0));
    std::fflush(stdout);
    core.shutdown();
    return 0;
}

}  // namespace tbcad::app
