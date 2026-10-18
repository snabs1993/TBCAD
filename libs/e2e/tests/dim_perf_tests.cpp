// How long a drawing full of dimensions takes: the first frame, a restyle that redraws every
// dimension, the whole-drawing edits that move associative dimensions along, DIMREGEN, and undo.
// Size: TBCAD_DIM_N (default 20,000 dimensions). Prints; the checks guard the results and, in an
// optimized build, generous limits.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

#ifdef NDEBUG
constexpr bool kOptimized = true;
#else
constexpr bool kOptimized = false;
#endif

struct Sheet {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    int n = 0;

    Sheet() {
        dir = std::filesystem::temp_directory_path() / "tbcad_dim_perf";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_DIM_N");
        n = env ? std::atoi(env) : (kOptimized ? 20000 : 4000);
        Document& doc = core->document();
        auto tx = doc.begin("gen");
        for (int i = 0; i < n; ++i) {
            const double x = (i % 200) * 400.0, y = (i / 200) * 300.0;
            auto add = [&](EntityData d) {
                Entity e;
                e.data = std::move(d);
                return tx.add(std::move(e)).value();
            };
            DimensionData d;
            switch (i % 5) {
                case 0:
                case 1: {
                    // A line with a linear or aligned dimension that follows its ends.
                    const EntityId l = add(LineData{{x, y}, {x + 120.5, y + 30}});
                    d.kind = i % 5 == 0 ? DimKind::Linear : DimKind::Aligned;
                    d.defPts = {Vec2{x, y}, Vec2{x + 120.5, y + 30}, Vec2{x + 60, y - 40}, Vec2{}};
                    d.assoc[0] = {DimAssoc::Kind::Vertex, l, 0};
                    d.assoc[1] = {DimAssoc::Kind::Vertex, l, 1};
                    break;
                }
                case 2:
                    (void)add(CircleData{{x + 50, y + 50}, 40});
                    d.kind = DimKind::Radius;
                    d.defPts = {Vec2{x + 50, y + 50}, Vec2{x + 90, y + 50}, Vec2{}, Vec2{}};
                    d.textPos = {x + 120, y + 70};
                    d.textPosUser = true;
                    break;
                case 3:
                    (void)add(CircleData{{x + 50, y + 50}, 40});
                    d.kind = DimKind::Diameter;
                    d.defPts = {Vec2{x + 10, y + 50}, Vec2{x + 90, y + 50}, Vec2{}, Vec2{}};
                    d.textPos = {x + 120, y + 70};
                    d.textPosUser = true;
                    break;
                default:
                    d.kind = DimKind::Angular3Point;
                    d.defPts = {Vec2{x + 80, y}, Vec2{x, y + 80}, Vec2{x, y}, Vec2{x + 40, y + 40}};
                    break;
            }
            dim::refresh(doc, d);
            (void)add(std::move(d));
        }
        tx.commit();
    }

    double frame() {
        auto t = std::chrono::steady_clock::now();
        core->updateFrame();
        int frames = 0;
        while (core->sceneBuilding() && frames < 100000) {
            core->updateFrame();
            ++frames;
        }
        return ms(t);
    }
    double command(std::initializer_list<const char*> lines) {
        auto t = std::chrono::steady_clock::now();
        for (const char* l : lines)
            core->submitText(l);
        frame();
        return ms(t);
    }
};

}  // namespace

TEST_CASE("perf: a drawing full of dimensions") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Sheet s;
    std::size_t dims = 0;
    s.core->document().forEachEntity(
        [&](const Entity& e) { dims += entityAs<DimensionData>(e) != nullptr; });
    REQUIRE(dims == static_cast<std::size_t>(s.n));
    std::printf("%d dimensions, %zu objects\n", s.n, s.core->document().modelSpaceCount());

    s.core->zoomExtents();
    const double first = s.frame();
    std::printf("  first frame (every dimension drawn): %.0f ms\n", first);
    CHECK(s.core->scene().entityCount() == s.core->document().modelSpaceCount());

    // Pure layout cost, without the scene.
    {
        auto t = std::chrono::steady_clock::now();
        std::size_t items = 0;
        s.core->document().forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<DimensionData>(e))
                items += dim::layoutDimension(s.core->document(), *d).lines.size();
        });
        std::printf("  layout of all dimensions: %.0f ms (%zu lines)\n", ms(t), items);
        CHECK(items > 0);
    }

    // Restyle: every dimension is drawn again.
    auto t0 = std::chrono::steady_clock::now();
    {
        auto tx = s.core->document().begin("restyle");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& d) { d.textHeight = 3.5; }).ok());
        tx.commit();
    }
    s.frame();
    const double restyle = ms(t0);
    std::printf("  text height 3.5, all dimensions redrawn: %.0f ms\n", restyle);

    s.core->selectAll();
    for (const auto& [label, lines] :
         std::initializer_list<std::pair<const char*, std::vector<const char*>>>{
             {"MOVE", {"MOVE", "0,0", "5,5"}},
             {"ROTATE", {"ROTATE", "0,0", "30"}},
             {"SCALE", {"SCALE", "0,0", "1.5"}},
             {"DIMREGEN", {"DIMREGEN"}},
         }) {
        s.core->selectAll();
        auto t = std::chrono::steady_clock::now();
        for (const char* l : lines)
            s.core->submitText(l);
        s.frame();
        const double run = ms(t);
        t = std::chrono::steady_clock::now();
        const bool undone = s.core->undo();
        s.frame();
        std::printf("  %-8s %6.0f ms   undo %6.0f ms\n", label, run, ms(t));
        CHECK(undone);
        if (kOptimized)
            CHECK(run < 15000);
    }

    // Panning over the drawing: one frame at full zoom-out after moving the view.
    {
        double worst = 0, sum = 0;
        for (int i = 0; i < 20; ++i) {
            s.core->panPixels(30, 20);
            auto t = std::chrono::steady_clock::now();
            s.core->updateFrame();
            const double m = ms(t);
            worst = std::max(worst, m);
            sum += m;
        }
        std::printf("  pan frames: mean %.1f ms, worst %.1f ms\n", sum / 20, worst);
        if (kOptimized)
            CHECK(sum / 20 < 100);
    }
    if (kOptimized)
        CHECK(first < 20000);
}
