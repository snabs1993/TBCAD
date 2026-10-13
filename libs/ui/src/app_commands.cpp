#include "app_commands.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

#include "tbcad/cmd/command.h"
#include "tbcad/geom/entity.h"
#include "tbcad/platform/paths.h"
#include "tbcad/render/camera.h"
#include "tbcad/tmpl/tmpl.h"
#include "tbcad/ui/settings_model.h"
#include "tbcad/ui/workspace.h"

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

class SaveAsTemplateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SAVEASTEMPLATE"; }
    std::string_view description() const override {
        return "Save the drawing as a template (.tbt) for new drawings";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions so;
        auto n = co_await ctx.getString("Template name", so);
        if (!n.ok())
            co_return;
        const std::string err = ctx.host().saveAsTemplate(cmd::trim(n.text));
        if (!err.empty())
            ctx.message(err);
    }
};

class NewFromTemplateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "NEWFROMTEMPLATE"; }
    std::vector<std::string> defaultAliases() const override { return {"QNEWT"}; }
    std::string_view description() const override {
        return "Start a new drawing from a template (Built-in, or a .tbt file)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string all;
        for (const std::string& t : ctx.host().templateNames())
            all += (all.empty() ? "" : ", ") + t;
        ctx.message("Templates: " + all);
        cmd::StringOptions so;
        so.defaultValue = tmpl::kBuiltinTemplate;
        auto n = co_await ctx.getString("Template name", so);
        if (!n.ok())
            co_return;
        // The window opens the drawing once this command has ended; a command must not swap the
        // document it is editing.
        ctx.host().requestUi("newfrom:" + cmd::trim(n.text));
    }
};

class TemplateDefaultCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEMPLATEDEFAULT"; }
    std::string_view description() const override {
        return "Choose the template that NEW starts from";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const std::vector<std::string> names = ctx.host().templateNames();
        std::string all;
        for (const std::string& t : names)
            all += (all.empty() ? "" : ", ") + t;
        ctx.message("Templates: " + all);
        cmd::StringOptions so;
        std::string current = ctx.host().setting("template.default").value_or("");
        so.defaultValue = current.empty() ? std::string(tmpl::kBuiltinTemplate) : current;
        auto n = co_await ctx.getString("Default template", so);
        if (!n.ok())
            co_return;
        const std::string want = cmd::trim(n.text);
        if (std::find(names.begin(), names.end(), want) == names.end()) {
            ctx.message("No template named \"" + want + "\".");
            co_return;
        }
        ctx.host().setSetting("template.default", want == tmpl::kBuiltinTemplate ? "" : want);
        ctx.message("New drawings start from " + want + ".");
    }
};

class WorkspaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WORKSPACE"; }
    std::vector<std::string> defaultAliases() const override { return {"WS"}; }
    std::string_view description() const override {
        return "Switch, save, rename or delete workspaces (saved panel layouts)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        WorkspaceStore store(ctx.host().settingsFolder() / "workspaces");
        cmd::KeywordOptions ko;
        ko.keywords = {"Load", "Save", "Rename", "Delete", "List"};
        ko.defaultKeyword = "Load";
        auto k = co_await ctx.getKeyword("Workspace option [Load/Save/Rename/Delete/List]", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "List") {
            std::string all;
            for (const std::string& n : store.names())
                all += (all.empty() ? "" : ", ") + n;
            ctx.message("Workspaces: " + all);
            co_return;
        }
        cmd::StringOptions so;
        auto n = co_await ctx.getString("Workspace name", so);
        if (!n.ok())
            co_return;
        const std::string name = cmd::trim(n.text);
        if (k.keyword == "Load") {
            if (!store.load(name))
                ctx.message("No workspace named \"" + name + "\".");
            else
                ctx.host().requestUi("workspace:load:" + name);  // the window owns the panels
        } else if (k.keyword == "Save") {
            if (store.isBuiltin(name))
                ctx.message("\"" + name + "\" is built in; choose another name.");
            else if (name.empty())
                ctx.message("Give the workspace a name.");
            else
                ctx.host().requestUi("workspace:save:" + name);
        } else if (k.keyword == "Delete") {
            const std::string err = store.remove(name);
            ctx.message(err.empty() ? "Workspace deleted." : err);
        } else {
            cmd::StringOptions no;
            auto to = co_await ctx.getString("New name", no);
            if (!to.ok())
                co_return;
            const std::string err = store.rename(name, cmd::trim(to.text));
            ctx.message(err.empty() ? "Workspace renamed." : err);
        }
    }
};

std::filesystem::path pathOf(const std::string& text) {
    return platform::pathFromUtf8(cmd::trim(text));
}

class AliasImportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ALIASIMPORT"; }
    std::string_view description() const override {
        return "Import command aliases from an acad.pgp style file";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions so;
        auto p = co_await ctx.getString("File to import (.pgp)", so);
        if (!p.ok())
            co_return;
        std::ifstream in(pathOf(p.text), std::ios::binary);
        if (!in) {
            ctx.message("Cannot read " + cmd::trim(p.text));
            co_return;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        AliasEditor editor(ctx.registry(), ctx.host().settingsFolder() / "aliases.json");
        AliasImportReport rep = editor.importPgp(text);
        if (!rep.error.empty()) {
            ctx.message("Aliases not imported: " + rep.error);
            co_return;
        }
        for (std::size_t i = 0; i < rep.skipped.size() && i < 8; ++i)
            ctx.message("Skipped " + rep.skipped[i]);
        if (rep.skipped.size() > 8)
            ctx.message(std::to_string(rep.skipped.size() - 8) + " more skipped.");
        ctx.message("Imported " + std::to_string(rep.added) + " alias(es).");
    }
};

class AliasExportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ALIASEXPORT"; }
    std::string_view description() const override {
        return "Write all command aliases to an acad.pgp style file";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions so;
        auto p = co_await ctx.getString("File to write (.pgp)", so);
        if (!p.ok())
            co_return;
        AliasEditor editor(ctx.registry(), {});
        std::ofstream out(pathOf(p.text), std::ios::binary | std::ios::trunc);
        out << editor.exportPgp();
        if (!out)
            ctx.message("Cannot write " + cmd::trim(p.text));
        else
            ctx.message("Aliases written to " + cmd::trim(p.text));
    }
};

class KeysFileCommand final : public cmd::Command {
public:
    KeysFileCommand(const char* name, bool import, const char* what)
        : name_(name), import_(import), what_(what) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return what_; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions so;
        auto p = co_await ctx.getString(
            import_ ? "Key binding file to read (.json)" : "Key binding file to write (.json)", so);
        if (!p.ok())
            co_return;
        // The window owns the key bindings; it does the work once this command has ended.
        ctx.host().requestUi(std::string(import_ ? "keys:import:" : "keys:export:") +
                             cmd::trim(p.text));
    }

private:
    const char* name_;
    bool import_;
    const char* what_;
};

}  // namespace

void registerAppCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ZoomCommand>());
    (void)registry.add(std::make_unique<PanCommand>());
    (void)registry.add(std::make_unique<DevGenCommand>());
    (void)registry.add(std::make_unique<SettingsCommand>());
    (void)registry.add(std::make_unique<SettingsPageCommand>());
    (void)registry.add(std::make_unique<SaveAsTemplateCommand>());
    (void)registry.add(std::make_unique<NewFromTemplateCommand>());
    (void)registry.add(std::make_unique<TemplateDefaultCommand>());
    (void)registry.add(std::make_unique<WorkspaceCommand>());
    (void)registry.add(std::make_unique<AliasImportCommand>());
    (void)registry.add(std::make_unique<AliasExportCommand>());
    (void)registry.add(std::make_unique<KeysFileCommand>("KEYSIMPORT", true,
                                                         "Load a key binding profile from a file"));
    (void)registry.add(
        std::make_unique<KeysFileCommand>("KEYSEXPORT", false, "Save the key bindings to a file"));
}

}  // namespace tbcad::ui
