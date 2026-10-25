#include "app_commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

void pushView(cmd::IHost& host, const cmd::ViewState& v) {
    cmd::pushViewHistory(host, v);
}

std::optional<cmd::ViewState> popView(cmd::IHost& host, const cmd::ViewState& like) {
    return cmd::popViewHistory(host, like);
}

// "2X", "0.5x" (times the current view) and "0.01XP", "1/100XP" (times paper space).
struct ZoomFactor {
    double value = 1;
    bool paper = false;
};
std::optional<ZoomFactor> parseZoomFactor(const std::string& text) {
    std::string t;
    for (char c : text)
        if (c != ' ')
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ZoomFactor f;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "xp") == 0) {
        f.paper = true;
        t.erase(t.size() - 2);
    } else if (t.size() > 1 && t.back() == 'x') {
        t.pop_back();
    } else {
        return std::nullopt;
    }
    const auto slash = t.find('/');
    char* end = nullptr;
    const std::string num = slash == std::string::npos ? t : t.substr(0, slash);
    double v = std::strtod(num.c_str(), &end);
    if (num.empty() || *end)
        return std::nullopt;
    if (slash != std::string::npos) {
        const std::string den = t.substr(slash + 1);
        const double d = std::strtod(den.c_str(), &end);
        if (den.empty() || *end || d == 0)
            return std::nullopt;
        v /= d;
    }
    if (!(v > 0))
        return std::nullopt;
    f.value = v;
    return f;
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
        opts.keywords = {"All",    "Center", "Dynamic", "Extents", "Previous", "Scale",
                         "Window", "Object", "Double",  "Half",    "Realtime"};
        opts.freeText = true;   // 0.5X, 1/100XP
        opts.allowNone = true;  // Enter: real time
        cmd::PointResult first;
        if (*preset_) {
            first.status = cmd::Status::Keyword;
            first.keyword = preset_;
        } else {
            first = co_await ctx.getPoint(
                "Specify corner of window, enter a scale factor (nX or nXP), or "
                "[All/Center/Dynamic/Extents/Previous/Scale/Window/Object] <real time>",
                opts);
        }
        if (first.status == cmd::Status::None ||
            (first.status == cmd::Status::Keyword && first.keyword == "Realtime")) {
            // Drag up to zoom in, down to zoom out; Esc or Enter ends it (the window does it).
            pushView(ctx.host(), ctx.host().view());
            ctx.host().requestUi("realtime:zoom");
            ctx.message("Press Esc or Enter to exit, or right-click to end real-time zoom.");
            co_return;
        }
        if (first.status == cmd::Status::Keyword) {
            const cmd::ViewState v = ctx.host().view();
            render::Camera2D cam = cameraOf(v);
            if (const auto f = parseZoomFactor(first.keyword)) {
                if (f->paper) {
                    ctx.host().requestUi("zoomxp:" + std::to_string(f->value));
                } else {
                    pushView(ctx.host(), v);
                    cam.zoomAt({cam.viewportW / 2.0, cam.viewportH / 2.0}, f->value);
                    ctx.host().setView(viewOf(cam, v));
                }
                co_return;
            }
            if (std::find(opts.keywords.begin(), opts.keywords.end(), first.keyword) ==
                opts.keywords.end()) {
                ctx.message("Invalid option \"" + first.keyword +
                            "\". Use a keyword, a factor like 0.5X, or a "
                            "paper scale like 0.01XP.");
                co_return;
            }
            if (first.keyword == "Previous") {
                if (auto prev = popView(ctx.host(), v))
                    ctx.host().setView(*prev);
                else
                    ctx.message("No previous view saved.");
                co_return;
            }
            if (first.keyword == "Dynamic") {
                // The whole drawing is shown with a box the size of the current view; place the
                // box, then size it (Enter keeps its size), and the view zooms to it.
                const BBox2 was = cam.visibleWorld();
                BBox2 all = ctx.host().viewExtents();
                if (all.empty())
                    all = boxOfEntities(ctx.doc());
                all.merge(was);
                pushView(ctx.host(), v);
                render::Camera2D wide = cam;
                wide.fit(all);
                ctx.host().setView(viewOf(wide, v));
                const double w = was.max.x - was.min.x, h = was.max.y - was.min.y;
                auto box = [](Vec2 c, double bw, double bh, DisplayList& out) {
                    DisplayPolyline pl;
                    pl.closed = true;
                    pl.style.color = Rgba{90, 170, 255, 255};
                    pl.pts = {{c.x - bw / 2, c.y - bh / 2},
                              {c.x + bw / 2, c.y - bh / 2},
                              {c.x + bw / 2, c.y + bh / 2},
                              {c.x - bw / 2, c.y + bh / 2}};
                    out.items.emplace_back(std::move(pl));
                };
                cmd::PointOptions po;
                po.allowNone = false;
                po.preview = [box, w, h](Vec2 cur, DisplayList& out) { box(cur, w, h, out); };
                auto c = co_await ctx.getPoint("Place the view box", po);
                if (!c.ok()) {
                    ctx.host().setView(v);
                    co_return;
                }
                const Vec2 centre = c.point;
                cmd::PointOptions so;
                so.allowNone = true;
                so.base = centre;
                const double aspect = h > 0 ? w / h : 1;
                auto sized = [aspect](Vec2 ctr, Vec2 cur) {
                    double bw = std::abs(cur.x - ctr.x) * 2, bh = std::abs(cur.y - ctr.y) * 2;
                    bw = std::max(bw, bh * aspect);
                    return std::pair<double, double>{bw, bw / aspect};
                };
                so.preview = [box, sized, centre](Vec2 cur, DisplayList& out) {
                    const auto [bw, bh] = sized(centre, cur);
                    box(centre, bw, bh, out);
                };
                auto corner = co_await ctx.getPoint("Size the view box <keep its size>", so);
                if (corner.status == cmd::Status::Cancel) {
                    ctx.host().setView(v);
                    co_return;
                }
                double bw = w, bh = h;
                if (corner.ok()) {
                    const auto sz = sized(centre, corner.point);
                    if (sz.first > 0) {
                        bw = sz.first;
                        bh = sz.second;
                    }
                }
                BBox2 target;
                target.expand({centre.x - bw / 2, centre.y - bh / 2});
                target.expand({centre.x + bw / 2, centre.y + bh / 2});
                cam.fit(target, 0.0);
                ctx.host().setView(viewOf(cam, v));
                co_return;
            }
            pushView(ctx.host(), v);
            if (first.keyword == "Extents" || first.keyword == "All") {
                BBox2 box = ctx.host().viewExtents();
                if (box.empty())
                    box = boxOfEntities(ctx.doc());
                if (first.keyword == "All")  // All covers the drawing limits as well
                    box.merge(drawingLimits(ctx.doc().header()));
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

// NAVBAR: the navigation bar at the right edge of the drawing (pan, zoom extents, window,
// previous, in and out). Setting "ui.navbar".
class NavBarCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "NAVBAR"; }
    std::string_view description() const override {
        return "Show or hide the navigation bar (pan and zoom buttons)";
    }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool on = ctx.host().setting("ui.navbar").value_or("1") != "0";
        cmd::KeywordOptions ko;
        ko.keywords = {"ON", "OFF"};
        ko.defaultKeyword = on ? "OFF" : "ON";
        auto k = co_await ctx.getKeyword(std::string("Navigation bar is ") + (on ? "on" : "off") +
                                             " [ON/OFF] <" + (on ? "OFF" : "ON") + ">",
                                         ko);
        if (k.ok())
            ctx.host().setSetting("ui.navbar", k.keyword == "ON" ? "1" : "0");
    }
};

// PAN [P]: real-time pan, the drawing follows the pointer while the left button is held; Esc or
// Enter ends it. -PAN moves the view by two points.
class RealtimePanCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAN"; }
    std::vector<std::string> defaultAliases() const override { return {"P"}; }
    std::string_view description() const override { return "Move the view with the mouse"; }
    bool isTransparent() const override { return true; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        pushView(ctx.host(), ctx.host().view());
        ctx.host().requestUi("realtime:pan");
        ctx.message("Press Esc or Enter to exit, or right-click to end real-time pan.");
        co_return;
    }
};

class PanCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-PAN"; }
    std::string_view description() const override { return "Move the view by two points"; }
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
        pushView(ctx.host(), v);
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

// CUI: the Customize page of Settings (Quick Access toolbar, right-click menu, double-click
// actions); command aliases and key bindings have their own pages next to it.
class CuiCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CUI"; }
    std::vector<std::string> defaultAliases() const override { return {"TOOLBAR", "QUICKCUI"}; }
    std::string_view description() const override {
        return "Customize the Quick Access toolbar, right-click menu and double-click actions";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi("settings:Customize");
        co_return;
    }
};

// TEXTSCR: the whole command history in a window (F2, as in AutoCAD).
class TextScrCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXTSCR"; }
    std::string_view description() const override {
        return "Show the command history in a window (F2)";
    }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi("action:view.textWindow");
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

// DSETTINGS [DS, OSETTINGS]: the Drafting page of the Settings dialog (snap, grid, polar, object
// snap, dynamic input).
class DraftingSettingsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DSETTINGS"; }
    std::vector<std::string> defaultAliases() const override { return {"DS", "OSETTINGS"}; }
    std::string_view description() const override {
        return "Drafting settings: snap, grid, polar tracking, object snap, dynamic input";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi("settings:Drafting");
        co_return;
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

// Commands that only ask the window to do something (the open drawings live there).
class UiRequestCommand final : public cmd::Command {
public:
    UiRequestCommand(const char* name, std::vector<std::string> aliases, const char* request,
                     const char* help)
        : name_(name), aliases_(std::move(aliases)), request_(request), help_(help) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    std::string_view description() const override { return help_; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi(request_);
        co_return;
    }

private:
    const char* name_;
    std::vector<std::string> aliases_;
    const char* request_;
    const char* help_;
};

// EXPORT [EXP]: one command for every export format, picked by the file's extension, as
// AutoCAD's Export Data dialog does. Hands over to EXPORTDXF / EXPORTPDF / EXPORTSVG /
// EXPORTPNG / EXPORTJPG with their defaults.
class ExportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXPORT"; }
    std::vector<std::string> defaultAliases() const override { return {"EXP"}; }
    std::string_view description() const override {
        return "Export the drawing to DXF, PDF, SVG, PNG or JPEG, chosen by the file name";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool dialog = ctx.host().dialogsAvailable() &&
                            ctx.host().setting("files.filedia").value_or("1") != "0";
        std::string stem = "Drawing1";
        if (auto f = ctx.host().setting("document.file"); f && !f->empty())
            stem = platform::pathToUtf8(platform::pathFromUtf8(*f).stem());
        if (dialog)
            ctx.host().requestUi("savefile:export:" + stem + ".pdf");
        cmd::StringOptions so;
        so.defaultValue = stem + ".pdf";
        auto r = co_await ctx.getString("Export to file (.dxf .pdf .svg .png .jpg)", so);
        if (!r.ok())
            co_return;
        std::string file = cmd::trim(r.text);
        if (file.size() > 1 && file.front() == '"' && file.back() == '"')
            file = file.substr(1, file.size() - 2);
        if (file.empty())
            co_return;
        std::string ext = platform::pathToUtf8(platform::pathFromUtf8(file).extension());
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext.empty()) {
            ext = ".pdf";
            file += ext;
        }
        const char* command = ext == ".dxf"                     ? "EXPORTDXF"
                              : ext == ".pdf"                   ? "EXPORTPDF"
                              : ext == ".svg"                   ? "EXPORTSVG"
                              : ext == ".png"                   ? "EXPORTPNG"
                              : ext == ".jpg" || ext == ".jpeg" ? "EXPORTJPG"
                                                                : nullptr;
        if (!command) {
            ctx.message("Cannot export to " + ext + " files. Use .dxf, .pdf, .svg, .png or .jpg.");
            co_return;
        }
        std::error_code ec;
        const bool exists = std::filesystem::exists(platform::pathFromUtf8(file), ec);
        if (exists && !dialog) {  // the file dialog has already asked
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = "No";
            auto answer = co_await ctx.getKeyword(file + " already exists. Replace it?", yn);
            if (answer.status != cmd::Status::Keyword || answer.keyword != "Yes") {
                ctx.message("Export cancelled; the existing file was not changed.");
                co_return;
            }
        }
        std::vector<std::string> lines{command};
        if (ext == ".dxf") {
            lines.push_back(file);
            if (exists)
                lines.emplace_back("Yes");  // already answered above or in the dialog
            lines.emplace_back("");         // default DXF version
        } else {
            lines.emplace_back("Export");
            lines.push_back(file);
            if (exists && ctx.host().setting("files.filedia").value_or("1") != "0")
                lines.emplace_back("Yes");
        }
        if (!ctx.host().queueScript(std::move(lines), 1))
            ctx.message("EXPORT cannot hand over to " + std::string(command) + " here.");
    }
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

// PICKADD / PICKFIRST: a 0 / 1 / 2 system variable kept in the host settings.
class PickVarCommand final : public cmd::Command {
public:
    PickVarCommand(const char* name, const char* key, int maxValue, int fallback, const char* help)
        : name_(name), key_(key), max_(maxValue), fallback_(fallback), help_(help) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return help_; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string cur = ctx.host().setting(key_).value_or(std::to_string(fallback_));
        cmd::StringOptions so;
        so.defaultValue = cur;
        auto r =
            co_await ctx.getString("New value for " + std::string(name_) + " <" + cur + ">", so);
        if (!r.ok())
            co_return;
        const std::string t = cmd::trim(r.text);
        if (t.size() != 1 || t[0] < '0' || t[0] - '0' > max_) {
            ctx.message("Type a value from 0 to " + std::to_string(max_) + ".");
            co_return;
        }
        ctx.host().setSetting(key_, t);
        ctx.message(std::string(name_) + " = " + t + ". " + help_);
    }

private:
    const char* name_;
    const char* key_;
    int max_;
    int fallback_;
    const char* help_;
};

// MSPACE / PSPACE: switch between editing the model through a viewport and the sheet.
class SpaceCommand final : public cmd::Command {
public:
    SpaceCommand(const char* name, const char* alias, const char* space, const char* description)
        : name_(name), alias_(alias), space_(space), description_(description) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override {
        return *alias_ ? std::vector<std::string>{alias_} : std::vector<std::string>{};
    }
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
        return "Start a new drawing from a template (Built-in, .tbt, .dxf or .dwt file)";
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
        std::error_code existsEc;
        const bool isFile =
            std::filesystem::is_regular_file(platform::pathFromUtf8(want), existsEc);
        if (!isFile && std::find(names.begin(), names.end(), want) == names.end()) {
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

// WSCURRENT: switches to a workspace by name (the current one is the default).
class WsCurrentCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WSCURRENT"; }
    std::string_view description() const override { return "Switch to a workspace by name"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        WorkspaceStore store(ctx.host().settingsFolder() / "workspaces");
        const std::string current = ctx.host().setting("workspace.current").value_or("");
        cmd::StringOptions so;
        if (!current.empty())
            so.defaultValue = current;
        const std::string prompt = current.empty()
                                       ? std::string("Enter new value for WSCURRENT")
                                       : "Enter new value for WSCURRENT <" + current + ">";
        auto n = co_await ctx.getString(prompt, std::move(so));
        if (!n.ok())
            co_return;
        const std::string name = cmd::trim(n.text);
        if (!store.load(name)) {
            std::string all;
            for (const std::string& w : store.names())
                all += (all.empty() ? "" : ", ") + w;
            ctx.message("No workspace named \"" + name + "\". Workspaces: " + all);
            co_return;
        }
        ctx.host().requestUi("workspace:load:" + name);
    }
};

// WSSAVE: saves the panels and their layout as a workspace (the current one by default).
class WsSaveCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WSSAVE"; }
    std::string_view description() const override {
        return "Save the panels and their layout as a workspace";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        WorkspaceStore store(ctx.host().settingsFolder() / "workspaces");
        std::string current = ctx.host().setting("workspace.current").value_or("");
        if (store.isBuiltin(current))
            current.clear();  // a built-in workspace cannot be overwritten
        cmd::StringOptions so;
        if (!current.empty())
            so.defaultValue = current;
        const std::string prompt = current.empty() ? std::string("Save workspace as")
                                                   : "Save workspace as <" + current + ">";
        auto n = co_await ctx.getString(prompt, std::move(so));
        if (!n.ok())
            co_return;
        const std::string name = cmd::trim(n.text);
        if (name.empty())
            ctx.message("Give the workspace a name.");
        else if (store.isBuiltin(name))
            ctx.message("\"" + name + "\" is built in; choose another name.");
        else
            ctx.host().requestUi("workspace:save:" + name);
    }
};

// CLEANSCREENON / CLEANSCREENOFF: the Clean Screen view (Ctrl+0 or F11) from the command line.
class CleanScreenCommand final : public cmd::Command {
public:
    explicit CleanScreenCommand(bool on) : on_(on) {}
    std::string_view name() const override { return on_ ? "CLEANSCREENON" : "CLEANSCREENOFF"; }
    std::string_view description() const override {
        return on_ ? "Hide the panels, menus and toolbars, leaving the drawing"
                   : "Bring back the panels, menus and toolbars";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().requestUi(on_ ? "cleanscreen:on" : "cleanscreen:off");
        co_return;
    }

private:
    bool on_;
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

// ALIASEDIT: list, add, change or remove one command alias from the command line (the
// Settings page edits the same table). Saved to aliases.json at once.
class AliasEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ALIASEDIT"; }
    std::string_view description() const override {
        return "List, add, change or remove command aliases (as acad.pgp does)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"List", "Set", "Remove"};
        ko.defaultKeyword = "List";
        auto act = co_await ctx.getKeyword("Alias [List/Set/Remove] <List>", ko);
        if (!act.ok())
            co_return;
        AliasEditor editor(ctx.registry(), ctx.host().settingsFolder() / "aliases.json");
        if (act.keyword == "List") {
            cmd::StringOptions opt;
            opt.allowEmpty = true;
            auto f = co_await ctx.getString("Command to list the aliases of (Enter for all)", opt);
            if (f.status == cmd::Status::Cancel)
                co_return;
            const std::string filter = f.ok() ? cmd::trim(f.text) : std::string();
            std::size_t n = 0;
            for (const cmd::AliasEntry& r : editor.rows()) {
                const std::string up = cmd::toUpperAscii(filter);
                if (!filter.empty() && cmd::toUpperAscii(r.command) != up &&
                    cmd::toUpperAscii(r.alias) != up)
                    continue;
                ctx.message(r.alias + " -> " + (r.command.empty() ? "(conflict)" : r.command) +
                            (r.note.empty() ? "" : "  " + r.note));
                ++n;
            }
            ctx.message(std::to_string(n) + " alias(es).");
            co_return;
        }
        auto a = co_await ctx.getString(act.keyword == "Set" ? "Alias to set" : "Alias to remove");
        if (!a.ok())
            co_return;
        const std::string alias = cmd::trim(a.text);
        if (act.keyword == "Remove") {
            const std::string err = editor.remove(alias);
            ctx.message(err.empty() ? "Alias " + alias + " removed." : err);
            co_return;
        }
        auto c = co_await ctx.getString("Command for " + alias);
        if (!c.ok())
            co_return;
        const std::string err = editor.set(alias, cmd::trim(c.text));
        ctx.message(err.empty() ? "Alias " + alias + " set." : err);
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
    (void)registry.add(std::make_unique<ExportCommand>());
    (void)registry.add(std::make_unique<UiRequestCommand>(
        "SAVEALL", std::vector<std::string>{}, "action:file.saveAll",
        "Save every open drawing that has changes"));
    (void)registry.add(std::make_unique<UiRequestCommand>(
        "CLOSEALL", std::vector<std::string>{}, "action:file.closeAll",
        "Close every open drawing, asking about unsaved changes"));
    (void)registry.add(std::make_unique<UiRequestCommand>(
        "QVDRAWING", std::vector<std::string>{"QVD"}, "action:view.quickViewDrawings",
        "Quick view of the open drawings and their layouts"));
    (void)registry.add(std::make_unique<UiRequestCommand>(
        "QVLAYOUT", std::vector<std::string>{"QVL"}, "action:view.quickViewLayouts",
        "Quick view of the layouts of this drawing"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "PICKADD", "selection.pickadd", 2, 2,
        "0 = a pick replaces the selection (Shift adds), 1 or 2 = picks add (Shift removes)"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "SELECTIONCYCLING", "selection.cycling", 2, 2,
        "With selection cycling on (SELCYC): 0 = off, 1 = click again for the next object under "
        "the cursor, 2 = a list of the overlapping objects to pick from"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "SELECTIONPREVIEW", "selection.preview", 1, 1,
        "1 = objects a selection box would pick are highlighted while you drag it, 0 = off"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "DBLCLKEDIT", "selection.dblclkedit", 1, 1,
        "1 = a double click edits the object (the action is set per type in Settings > "
        "Customize), 0 = it only selects"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "XREFNOTIFY", "xref.notify", 3, 2,
        "When the file of an external reference changes: 0 = say nothing, 1 = a marker in the "
        "status bar, 2 = the marker and a balloon to reload it, 3 = reload it by itself"));
    (void)registry.add(std::make_unique<PickVarCommand>(
        "PICKFIRST", "selection.pickfirst", 1, 1,
        "1 = commands use the objects selected before them, 0 = they always ask"));
    (void)registry.add(std::make_unique<SpaceCommand>(
        "MSPACE", "MS", "model", "Edit the model through the selected viewport of a layout"));
    (void)registry.add(std::make_unique<SpaceCommand>(
        "PSPACE", "PS", "paper", "Go back to the layout sheet from model space"));
    (void)registry.add(std::make_unique<SpaceCommand>(
        "VPMAX", "", "model", "Maximise the selected viewport of a layout to the whole window"));
    (void)registry.add(std::make_unique<SpaceCommand>(
        "VPMIN", "", "paper", "Restore a maximised viewport and show the layout sheet again"));
    (void)registry.add(std::make_unique<ZoomCommand>("ZE", "Extents"));
    (void)registry.add(std::make_unique<PanCommand>());
    (void)registry.add(std::make_unique<RealtimePanCommand>());
    (void)registry.add(std::make_unique<NavBarCommand>());
    (void)registry.add(std::make_unique<DevGenCommand>());
    (void)registry.add(std::make_unique<SettingsCommand>());
    (void)registry.add(std::make_unique<CuiCommand>());
    (void)registry.add(std::make_unique<TextScrCommand>());
    (void)registry.add(std::make_unique<SettingsPageCommand>());
    (void)registry.add(std::make_unique<DraftingSettingsCommand>());
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
    (void)registry.add(std::make_unique<WsCurrentCommand>());
    (void)registry.add(std::make_unique<WsSaveCommand>());
    (void)registry.add(std::make_unique<CleanScreenCommand>(true));
    (void)registry.add(std::make_unique<CleanScreenCommand>(false));
    (void)registry.add(std::make_unique<AliasImportCommand>());
    (void)registry.add(std::make_unique<AliasExportCommand>());
    (void)registry.add(std::make_unique<AliasEditCommand>());
    (void)registry.add(std::make_unique<KeysFileCommand>("KEYSIMPORT", true,
                                                         "Load a key binding profile from a file"));
    (void)registry.add(
        std::make_unique<KeysFileCommand>("KEYSEXPORT", false, "Save the key bindings to a file"));
}

}  // namespace tbcad::ui
