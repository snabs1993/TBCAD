// Frame time of the whole window with a big drawing: 393,000 lines (the size of the owner's
// Topo drawing), thousands of layers, hundreds of blocks and inserts, many layouts and a long
// command history, with everything selected. Every panel and dialog is opened in turn and what it
// adds to a frame is measured against the frame without it. A panel may add at most 1 ms (median)
// in optimised builds; unoptimised builds only report, since they are several times slower.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "gui_harness.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

constexpr double kPanelBudgetMs = 1.0;  // median extra time per frame for one open panel
// The window without panels: menus, ribbon, tabs, status.
[[maybe_unused]] constexpr double kShellBudgetMs = 8.0;

void bigDrawing(Harness& h) {
    ui::AppCore& core = h.core();
    // Autosave encodes the whole drawing on this thread once in a while; keep it out of the
    // numbers.
    core.setSetting("autosave.enabled", "0");
    core.submitText("DEVGEN LINES 393000");
    Document& doc = core.document();
    Transaction tx = doc.begin("perf drawing");
    std::vector<LayerId> layers;
    for (int i = 0; i < 2000; ++i) {
        Layer l;
        l.name = "A-LAYER-" + std::to_string(i);
        l.color = Color::index(static_cast<std::uint8_t>(1 + i % 250));
        l.on = i % 7 != 0;
        l.frozen = i % 11 == 0;
        layers.push_back(tx.addLayer(std::move(l)).value());
    }
    std::vector<BlockId> blocks;
    for (int b = 0; b < 300; ++b) {
        BlockDef def;
        def.name = "BLOCK-" + std::to_string(b);
        const BlockId id = tx.addBlock(std::move(def)).value();
        for (int k = 0; k < 5; ++k)
            (void)tx.add(Entity{{}, LineData{{0, double(k)}, {10, double(k) + 3}}}, id);
        blocks.push_back(id);
    }
    for (int i = 0; i < 1000; ++i) {
        InsertData ins;
        ins.block = blocks[static_cast<std::size_t>(i) % blocks.size()];
        ins.position = {double(i % 40) * 50, double(i / 40) * 50};
        Entity e{{}, std::move(ins)};
        e.common.layer = layers[static_cast<std::size_t>(i) % layers.size()];
        (void)tx.add(std::move(e));
    }
    for (int i = 0; i < 2000; ++i) {
        TextData t;
        t.position = {double(i % 50) * 20, -double(i / 50) * 10};
        t.utf8 = "Room " + std::to_string(i);
        Entity e{{}, std::move(t)};
        e.common.layer = layers[static_cast<std::size_t>(i * 7) % layers.size()];
        (void)tx.add(std::move(e));
    }
    tx.commit();
    for (int i = 0; i < 40; ++i) {
        core.submitText("LAYOUT N Sheet" + std::to_string(i) + " A3");
        core.pressEscape();
    }
    for (int i = 0; i < 2500; ++i)
        core.submitText("NOTACOMMAND" + std::to_string(i));
    core.selectAll();
}

double medianFrameMs(Harness& h, int frames) {
    std::vector<double> ms;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        h.frame();
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

}  // namespace

TEST_CASE("frame time with a 393k drawing selected: the shell and every panel and dialog") {
    Harness h(1920, 1080);
    bigDrawing(h);
    REQUIRE(h.core().selection().size() > 390000);
    // Panels the default layout opens are closed first, so the baseline is the bare window.
    for (const ui::PanelInfo& p : h.ui().panels())
        h.core().closePanel(p.id);
    h.frame(2);
    // The first frames build the view's scene a slice at a time (about 80 frames for this
    // drawing); measure after that.
    h.frame(150);
    double shell = 0;
    shell = medianFrameMs(h, 9);
    MESSAGE("window without panels: " << shell << " ms");

    std::vector<std::pair<std::string, double>> over;
    auto measure = [&](const std::string& what, double extra) {
        MESSAGE(what << ": +" << extra << " ms (selected " << h.core().selection().size()
                     << ", items " << h.labels().size() << ")");
        if (extra > kPanelBudgetMs)
            over.emplace_back(what, extra);
    };
    for (const ui::PanelInfo& p : h.ui().panels()) {
        if (!p.draw)
            continue;
        h.core().openPanel(p.id);
        h.frame(3);
        measure("panel " + p.id, medianFrameMs(h, 7) - shell);
        h.core().closePanel(p.id);
        h.frame(2);
    }
    // Windows the shell itself owns.
    for (const char* what : {"action:view.textWindow", "settings", "drawingrecovery"}) {
        h.core().requestUi(what);
        h.frame(3);
        measure(what, medianFrameMs(h, 7) - shell);
        h.key(ImGuiKey_Escape);
        h.frame(3);
        h.core().selectAll();  // Escape also clears the selection
    }
    for (const auto& [what, ms] : over)
        MESSAGE("over budget: " << what << " adds " << ms << " ms per frame");
#ifdef NDEBUG
    CHECK_MESSAGE(shell <= kShellBudgetMs, "the window takes " << shell << " ms per frame");
    CHECK_MESSAGE(over.empty(), over.size() << " panels or dialogs are over budget");
#endif
}
