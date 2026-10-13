// The M1 gate: 1 000 000 random lines, zoom, pan, pick, window/crossing selection, save, reopen.
// Prints one line per measurement and PASS/FAIL against the milestone limits.
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "app.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/entity.h"

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
    core.submitText("DEVGEN LINES 1000000");
    double genMs = msSince(t0);
    const std::size_t count = core.document().modelSpaceCount();
    rep.line("generate 1 000 000 lines", genMs, "ms", "info", count == 1'000'000);
    rep.info("lines in drawing: " + std::to_string(count) + ", in index: " +
             std::to_string(core.spatialIndex().size()) + ", in scene: " +
             std::to_string(core.scene().entityCount()));

    // 2. Zoom extents, then draw.
    t0 = Clock::now();
    core.zoomExtents();
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
    rep.line("pan/zoom, median fps", 1000.0 / medianMs, "fps", ">= 30 (60 goal)", 1000.0 / medianMs >= 30);
    rep.line("pan/zoom, 5% worst frame fps", 1000.0 / p95Ms, "fps", ">= 20", 1000.0 / p95Ms >= 20);
    core.setCamera(start);

    // 4. Click selects the line under the cursor.
    {
        EntityId target = core.document().modelSpace()[500'000];
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
        t0 = Clock::now();
        auto opened = core.openFile(file);
        double openMs = msSince(t0);
        const bool same = opened.ok() && canonicalDump(core.document()) == before;
        rep.line("reopen (includes building the view)", openMs, "ms", "< 3000", opened.ok() && openMs < 3000);
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

}  // namespace tbcad::app
