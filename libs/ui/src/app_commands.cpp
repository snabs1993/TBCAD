#include "app_commands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>

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
    // Every kind counts (text, hatches, dimensions, inserts ...), so ZOOM Extents agrees with the
    // toolbar button, which uses the spatial index built from the same bounds.
    doc.forEachEntity([&](const Entity& e) {
        if (!geom::countsForExtents(e))
            return;
        BBox2 b = geom::bounds(e, doc);
        if (!b.empty())
            box.merge(b);
    });
    return box;
}

// Views before each ZOOM change, newest last, for ZOOM Previous ("x y pixelSize" per line).
constexpr const char* kViewHistoryKey = "tmp.view.history";
constexpr std::size_t kViewHistoryMax = 10;

void pushView(cmd::IHost& host, const cmd::ViewState& v) {
    std::string h = host.setting(kViewHistoryKey).value_or("");
    char line[128];
    std::snprintf(line, sizeof line, "%.12g %.12g %.12g\n", v.center.x, v.center.y, v.scale);
    h += line;
    // Keep the newest entries only.
    std::size_t lines = static_cast<std::size_t>(std::count(h.begin(), h.end(), '\n'));
    while (lines-- > kViewHistoryMax)
        h.erase(0, h.find('\n') + 1);
    host.setSetting(kViewHistoryKey, h);
}

std::optional<cmd::ViewState> popView(cmd::IHost& host, const cmd::ViewState& like) {
    std::string h = host.setting(kViewHistoryKey).value_or("");
    if (h.empty())
        return std::nullopt;
    const std::size_t end = h.size() - 1;  // the final '\n'
    const std::size_t start = h.rfind('\n', end - 1);
    const std::string last = h.substr(start == std::string::npos ? 0 : start + 1);
    h.erase(start == std::string::npos ? 0 : start + 1);
    host.setSetting(kViewHistoryKey, h);
    cmd::ViewState v = like;
    if (std::sscanf(last.c_str(), "%lf %lf %lf", &v.center.x, &v.center.y, &v.scale) != 3)
        return std::nullopt;
    return v;
}

class ZoomCommand final : public cmd::Command {
public:
    // `preset`: a ZOOM option applied without asking (ZE = ZOOM Extents).
    explicit ZoomCommand(const char* name = "ZOOM", const char* preset = "")
        : name_(name), preset_(preset) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override {
        return *preset_ ? std::vector<std::string>{} : std::vector<std::string>{"Z"};
    }
    std::string_view description() const override {
        return *preset_ ? "Zoom to the whole drawing" : "Change the magnification of the view";
    }
    bool isTransparent() const override { return true; }

private:
    const char* name_;
    const char* preset_;

public:
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::PointOptions opts;
        opts.keywords = {"Extents", "All",    "Window", "Previous", "Center",
                         "Scale",   "Object", "Double", "Half"};
        cmd::PointResult first;
        if (*preset_) {
            first.status = cmd::Status::Keyword;
            first.keyword = preset_;
        } else {
            first = co_await ctx.getPoint(
                "Specify first corner of the window or "
                "[Extents/All/Window/Previous/Center/Scale/Object/Double/Half]",
                opts);
        }
        if (first.status == cmd::Status::Keyword) {
            const cmd::ViewState v = ctx.host().view();
            render::Camera2D cam = cameraOf(v);
            if (first.keyword == "Previous") {
                if (auto prev = popView(ctx.host(), v))
                    ctx.host().setView(*prev);
                else
                    ctx.message("No previous view.");
                co_return;
            }
            pushView(ctx.host(), v);
            if (first.keyword == "Extents" || first.keyword == "All") {
                BBox2 box = ctx.host().viewExtents();
                if (box.empty())
                    box = boxOfEntities(ctx.doc());
                if (box.empty()) {
                    ctx.message("Nothing to zoom to.");
                    co_return;
                }
                cam.fit(box);
            } else if (first.keyword == "Center") {
                auto c = co_await ctx.getPoint("Specify center point");
                if (!c.ok())
                    co_return;
                cmd::RealOptions ro;
                ro.defaultValue = cam.viewportH * cam.pixelSizeMm();
                ro.min = 1e-9;
                auto h = co_await ctx.getReal("Enter magnification or height", ro);
                if (!h.ok())
                    co_return;
                cmd::ViewState nv = v;
                nv.center = c.point;
                nv.scale = h.value / std::max<double>(cam.viewportH, 1.0);
                ctx.host().setView(nv);
                co_return;
            } else if (first.keyword == "Scale") {
                cmd::RealOptions ro;
                ro.defaultValue = 1;
                ro.min = 1e-6;
                auto f =
                    co_await ctx.getReal("Enter a scale factor (n = times the current view)", ro);
                if (!f.ok())
                    co_return;
                cam.zoomAt({cam.viewportW / 2.0, cam.viewportH / 2.0}, f.value);
            } else if (first.keyword == "Object") {
                cmd::SelectionOptions so;
                so.allowPreselection = true;
                auto sel = co_await ctx.getSelection("Select objects", so);
                if (!sel.ok())
                    co_return;
                BBox2 box;
                for (EntityId id : sel.ids)
                    if (const Entity* e = ctx.doc().find(id)) {
                        const BBox2 b = geom::bounds(*e, ctx.doc());
                        if (!b.empty())
                            box.merge(b);
                    }
                if (box.empty()) {
                    ctx.message("Nothing to zoom to.");
                    co_return;
                }
                cam.fit(box);
            } else if (first.keyword == "Double") {
                cam.zoomAt({cam.viewportW / 2.0, cam.viewportH / 2.0}, 2.0);
            } else if (first.keyword == "Half") {
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
        pushView(ctx.host(), v);
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

// NEW, OPEN, SAVE, SAVEAS, CLOSE typed on the command line do what the File menu does: the window
// owns the dialogs, so the command only asks it ("action:file.save").
class FileActionCommand final : public cmd::Command {
public:
    // `pathPrompt`: the question asked on the command line when FILEDIA is 0 ("" = never asks).
    FileActionCommand(const char* name, const char* action, const char* description,
                      const char* pathPrompt = "")
        : name_(name), action_(action), description_(description), pathPrompt_(pathPrompt) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool open = std::string_view(action_) == "file.open";
        // Plain SAVE only needs a name for a drawing that has none yet.
        const bool needsName =
            open || std::string_view(action_) == "file.saveAs" || ctx.host().documentFile().empty();
        const bool dialogs = ctx.host().setting("files.filedia").value_or("1") != "0";
        if (*pathPrompt_ && needsName && !dialogs) {
            auto path = co_await ctx.getString(pathPrompt_);
            if (!path.ok())
                co_return;
            std::string text = cmd::trim(path.text);
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            if (text.empty())
                co_return;
            ctx.host().requestUi(std::string(open ? "openpath:" : "saveaspath:") + text);
            co_return;
        }
        ctx.host().requestUi(std::string("action:") + action_);
        co_return;
    }

private:
    const char* name_;
    const char* action_;
    const char* description_;
    const char* pathPrompt_;
};

// FILEDIA: 1 shows file dialogs, 0 asks for file names on the command line.
class FilediaCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FILEDIA"; }
    std::string_view description() const override {
        return "Choose file dialogs (1) or file names typed on the command line (0)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool on = ctx.host().setting("files.filedia").value_or("1") != "0";
        cmd::StringOptions so;
        so.defaultValue = on ? "1" : "0";
        auto v = co_await ctx.getString(
            "New value for FILEDIA (1 = dialogs, 0 = command line) <" + *so.defaultValue + ">", so);
        if (!v.ok())
            co_return;
        const std::string t = cmd::trim(v.text);
        if (t != "0" && t != "1") {
            ctx.message("Type 0 or 1.");
            co_return;
        }
        ctx.host().setSetting("files.filedia", t);
        ctx.message(t == "0" ? "OPEN, SAVEAS and PLOT now ask for the file name here."
                             : "OPEN and SAVEAS show the file dialog.");
    }
};

// MSPACE / PSPACE: switch between editing the model through a viewport and the sheet.
class SpaceCommand final : public cmd::Command {
public:
    SpaceCommand(const char* name, const char* alias, const char* space, const char* description)
        : name_(name), alias_(alias), space_(space), description_(description) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi(std::string("space:") + space_);
        co_return;
    }

private:
    const char* name_;
    const char* alias_;
    const char* space_;
    const char* description_;
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
    (void)registry.add(std::make_unique<FilediaCommand>());
    (void)registry.add(std::make_unique<SpaceCommand>(
        "MSPACE", "MS", "model", "Edit the model through the selected viewport of a layout"));
    (void)registry.add(std::make_unique<SpaceCommand>(
        "PSPACE", "PS", "paper", "Go back to the layout sheet from model space"));
    (void)registry.add(std::make_unique<ZoomCommand>("ZE", "Extents"));
    (void)registry.add(std::make_unique<PanCommand>());
    (void)registry.add(std::make_unique<DevGenCommand>());
    (void)registry.add(std::make_unique<SettingsCommand>());
    (void)registry.add(std::make_unique<SettingsPageCommand>());
    (void)registry.add(
        std::make_unique<FileActionCommand>("NEW", "file.new", "Start a new drawing"));
    (void)registry.add(std::make_unique<FileActionCommand>(
        "OPEN", "file.open", "Open a drawing file", "File name to open"));
    (void)registry.add(std::make_unique<FileActionCommand>(
        "SAVE", "file.save", "Save the drawing (asks for a name the first time)",
        "Save as (file name)"));
    (void)registry.add(std::make_unique<FileActionCommand>(
        "QSAVE", "file.save", "Save the drawing (same as SAVE)", "Save as (file name)"));
    (void)registry.add(std::make_unique<FileActionCommand>(
        "SAVEAS", "file.saveAs", "Save the drawing under another name or format",
        "Save as (file name)"));
    (void)registry.add(
        std::make_unique<FileActionCommand>("CLOSE", "file.close", "Close the current drawing"));
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
