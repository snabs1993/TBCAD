#include "app_commands.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "tbcad/cmd/command.h"
#include "tbcad/geom/entity.h"
#include "tbcad/render/camera.h"

namespace tbcad::ui {

namespace {

render::Camera2D cameraOf(const cmd::ViewState& v) {
    render::Camera2D cam;
    cam.center = v.center;
    cam.pixelsPerMm = v.scale > 0 ? 1.0 / v.scale : 1.0;
    cam.viewportW = std::max(1, static_cast<int>(v.sizePx.x));
    cam.viewportH = std::max(1, static_cast<int>(v.sizePx.y));
    return cam;
}

cmd::ViewState viewOf(const render::Camera2D& cam, const cmd::ViewState& like) {
    cmd::ViewState v = like;
    v.center = cam.center;
    v.scale = cam.pixelSizeMm();
    return v;
}

BBox2 boxOfEntities(const Document& doc) {
    BBox2 box;
    doc.forEachEntity([&](const Entity& e) {
        BBox2 b = geom::bounds(e, doc);
        if (!b.empty()) {
            box.expand(b.min);
            box.expand(b.max);
        }
    });
    return box;
}

class ZoomCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ZOOM"; }
    std::vector<std::string> defaultAliases() const override { return {"Z"}; }
    std::string_view description() const override { return "Change the magnification of the view"; }
    bool isTransparent() const override { return true; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::PointOptions opts;
        opts.keywords = {"Extents", "Window", "In", "Out"};
        auto first = co_await ctx.getPoint(
            "Specify first corner of the window or [Extents/Window/In/Out]", opts);
        if (first.status == cmd::Status::Keyword) {
            const cmd::ViewState v = ctx.host().view();
            render::Camera2D cam = cameraOf(v);
            if (first.keyword == "Extents") {
                BBox2 box = boxOfEntities(ctx.doc());
                if (box.empty()) {
                    ctx.message("Nothing to zoom to.");
                    co_return;
                }
                cam.fit(box);
            } else if (first.keyword == "In") {
                cam.zoomAt({cam.viewportW / 2.0, cam.viewportH / 2.0}, 2.0);
            } else if (first.keyword == "Out") {
                cam.zoomAt({cam.viewportW / 2.0, cam.viewportH / 2.0}, 0.5);
            } else {
                // Window: ask for the corners.
                auto a = co_await ctx.getPoint("Specify first corner");
                if (!a.ok())
                    co_return;
                cmd::PointOptions second;
                second.base = a.point;
                auto b = co_await ctx.getPoint("Specify opposite corner", second);
                if (!b.ok())
                    co_return;
                BBox2 box;
                box.expand(a.point);
                box.expand(b.point);
                cam.fit(box, 0.0);
            }
            ctx.host().setView(viewOf(cam, v));
            co_return;
        }
        if (!first.ok())
            co_return;
        cmd::PointOptions second;
        second.base = first.point;
        auto b = co_await ctx.getPoint("Specify opposite corner", second);
        if (!b.ok())
            co_return;
        const cmd::ViewState v = ctx.host().view();
        render::Camera2D cam = cameraOf(v);
        BBox2 box;
        box.expand(first.point);
        box.expand(b.point);
        cam.fit(box, 0.0);
        ctx.host().setView(viewOf(cam, v));
    }
};

class PanCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAN"; }
    std::vector<std::string> defaultAliases() const override { return {"P"}; }
    std::string_view description() const override { return "Move the view"; }
    bool isTransparent() const override { return true; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("Specify base point");
        if (!a.ok())
            co_return;
        cmd::PointOptions next;
        next.base = a.point;
        auto b = co_await ctx.getPoint("Specify second point", next);
        if (!b.ok())
            co_return;
        cmd::ViewState v = ctx.host().view();
        v.center = v.center + (a.point - b.point);
        ctx.host().setView(v);
    }
};

// DEVGEN LINES <count>: a reproducible random drawing for the performance gate. No description,
// so it stays out of the command list.
class DevGenCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DEVGEN"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions kopts;
        kopts.keywords = {"Lines"};
        kopts.defaultKeyword = "Lines";
        auto kind = co_await ctx.getKeyword("Generator [Lines]", kopts);
        if (!kind.ok())
            co_return;
        cmd::IntOptions iopts;
        iopts.min = 1;
        iopts.max = 20'000'000;
        iopts.defaultValue = 1'000'000;
        auto count = co_await ctx.getInteger("Number of lines", iopts);
        if (!count.ok())
            co_return;

        // 100 m x 100 m, lines of 50..500 mm, a few colours; fixed seed so every run is the same.
        constexpr double kSide = 100'000.0;
        std::uint64_t state = 0x9E3779B97F4A7C15ull;
        auto next = [&state]() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<double>(state >> 11) / static_cast<double>(1ull << 53);
        };
        auto& tx = ctx.tx();
        for (long long i = 0; i < count.value; ++i) {
            Vec2 a{next() * kSide, next() * kSide};
            double len = 50.0 + next() * 450.0;
            double ang = next() * kTwoPi;
            Entity e{{}, LineData{a, a + Vec2{std::cos(ang) * len, std::sin(ang) * len}}};
            if (i % 4 == 0)
                e.common.color = Color::index(static_cast<std::uint8_t>(1 + (i / 4) % 7));
            if (auto r = tx.add(std::move(e)); !r) {
                ctx.message(r.error().message);
                co_return;
            }
        }
        ctx.setUndoLabel("Generate lines");
        ctx.message("Generated " + std::to_string(count.value) + " lines.");

        const cmd::ViewState v = ctx.host().view();
        render::Camera2D cam = cameraOf(v);
        BBox2 box;
        box.expand({0, 0});
        box.expand({kSide, kSide});
        cam.fit(box);
        ctx.host().setView(viewOf(cam, v));
    }
};

class SettingsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SETTINGS"; }
    std::vector<std::string> defaultAliases() const override { return {"OPTIONS", "OP"}; }
    std::string_view description() const override {
        return "Open the Settings dialog (aliases, key bindings, display, files)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi("settings");
        co_return;
    }
};

// Hidden (no description): opens the Settings dialog on one page, for scripts and screenshots.
class SettingsPageCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SETTINGSPAGE"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions o;
        o.keywords = {"General", "Display", "Drafting", "Files", "Aliases", "Keybindings"};
        o.defaultKeyword = "General";
        auto r = co_await ctx.getKeyword(
            "Page [General/Display/Drafting/Files/Aliases/Keybindings]", std::move(o));
        if (r.ok())
            ctx.host().requestUi("settings:" + r.keyword);
    }
};

}  // namespace

void registerAppCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ZoomCommand>());
    (void)registry.add(std::make_unique<PanCommand>());
    (void)registry.add(std::make_unique<DevGenCommand>());
    (void)registry.add(std::make_unique<SettingsCommand>());
    (void)registry.add(std::make_unique<SettingsPageCommand>());
}

}  // namespace tbcad::ui
