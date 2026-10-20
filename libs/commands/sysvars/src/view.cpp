// VIEW opens the View Manager; -VIEW works on the command line. A named view is the part of the
// drawing shown (centre, size and zoom), optionally with the layers' on/frozen state at the time
// (a layer snapshot). Restoring one on a layout sheet sets the selected (or only) viewport.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/layout/layout.h"
#include "views.h"

namespace tbcad::sysvars {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

std::optional<NamedView> decode(const std::string& name, const std::string& text) {
    NamedView v;
    v.name = name;
    std::vector<std::string> fields;
    std::size_t from = 0;
    for (std::size_t t; (t = text.find('\t', from)) != std::string::npos; from = t + 1)
        fields.push_back(text.substr(from, t - from));
    fields.push_back(text.substr(from));
    std::istringstream in(fields[0]);
    if (!(in >> v.center.x >> v.center.y >> v.scale) || !(v.scale > 0))
        return std::nullopt;
    double w = 0, h = 0;
    if (in >> w >> h && w > 0 && h > 0) {
        v.width = w;
        v.height = h;
    }
    for (std::size_t i = 1; i < fields.size(); ++i) {
        if (fields[i] == "L")
            v.layers = true;
        else if (!fields[i].empty() && fields[i][0] == 'C')
            v.category = fields[i].substr(1);
    }
    return v;
}

// The key of the view named `name` (any case) or "".
std::string keyOf(const Document& doc, const std::string& name) {
    const std::string want = lower(name);
    for (const auto& [key, value] : doc.header().custom)
        if (key.rfind(kViewPrefix, 0) == 0 &&
            lower(key.substr(std::string_view(kViewPrefix).size())) == want)
            return key;
    return {};
}

// "Layout:id" of the viewport a restore on a sheet goes into: the selected one, else the only one.
int sheetViewport(cmd::IHost& host, const layout::Layout& lay) {
    const std::string sel = host.setting("tmp.layout.viewport").value_or("");
    if (const auto colon = sel.find(':');
        colon != std::string::npos && sel.substr(0, colon) == lay.name)
        if (lay.findViewport(std::atoi(sel.c_str() + colon + 1)))
            return std::atoi(sel.c_str() + colon + 1);
    return lay.viewports.size() == 1 ? lay.viewports.front().id : 0;
}

class ViewManagerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"V"}; }
    std::string_view description() const override {
        return "Open the View Manager: save, restore and change named views";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("views");
        co_return;
    }
};

class ViewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-VIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"-V", "NAMEDVIEW"}; }
    std::string_view description() const override {
        return "Save the current view under a name, restore a saved view, delete or list them";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Restore", "Save",     "Window",   "Delete", "Rename",
                       "LAyers",  "Category", "sEttings", "List"};
        ko.defaultKeyword = "Restore";
        auto k = co_await ctx.getKeyword(
            "View [Restore/Save/Window/Delete/Rename/LAyers/Category/sEttings/List] <Restore>", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "List") {
            const auto all = namedViews(ctx.doc());
            for (const NamedView& v : all)
                ctx.message(v.name + "  centre " + num(v.center.x) + "," + num(v.center.y) +
                            (v.width > 0 ? "  " + num(v.width) + " x " + num(v.height) : "") +
                            (v.layers ? "  layer snapshot" : "") +
                            (v.category.empty() ? "" : "  [" + v.category + "]"));
            if (all.empty())
                ctx.message("There are no saved views.");
            co_return;
        }
        if (k.keyword == "sEttings") {
            const bool on = ctx.host().setting(kViewLayerSnapshotKey).value_or("1") != "0";
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = on ? "Yes" : "No";
            auto a = co_await ctx.getKeyword(
                std::string("Save a layer snapshot with new views [Yes/No] <") +
                    (on ? "Yes" : "No") + ">",
                yn);
            if (a.ok())
                ctx.host().setSetting(kViewLayerSnapshotKey, a.keyword == "Yes" ? "1" : "0");
            co_return;
        }
        auto nameIn = co_await ctx.getString("View name");
        if (!nameIn.ok())
            co_return;
        const std::string name(cmd::trim(nameIn.text));
        if (name.empty())
            co_return;
        for (char c : name)
            if (static_cast<unsigned char>(c) < 0x20) {
                ctx.message("A view name cannot contain control characters.");
                co_return;
            }
        const std::string key = keyOf(ctx.doc(), name);
        std::optional<NamedView> existing;
        if (!key.empty())
            existing = decode(key.substr(std::string_view(kViewPrefix).size()),
                              ctx.doc().header().custom.at(key));
        auto missing = [&] { ctx.message("There is no view named \"" + name + "\"."); };

        if (k.keyword == "Save" || k.keyword == "Window") {
            const cmd::ViewState cur = ctx.host().view();
            NamedView v;
            v.name = existing ? existing->name : name;
            v.category = existing ? existing->category : std::string();
            v.center = cur.center;
            v.scale = cur.scale;
            v.width = cur.scale * cur.sizePx.x;
            v.height = cur.scale * cur.sizePx.y;
            if (k.keyword == "Window") {
                auto a = co_await ctx.getPoint("Specify first corner");
                if (!a.ok())
                    co_return;
                cmd::PointOptions po;
                po.base = a.point;
                auto b = co_await ctx.getPoint("Specify opposite corner", po);
                if (!b.ok())
                    co_return;
                v.width = std::abs(b.point.x - a.point.x);
                v.height = std::abs(b.point.y - a.point.y);
                if (!(v.width > 0) || !(v.height > 0)) {
                    ctx.message("The window has no area.");
                    co_return;
                }
                v.center = {(a.point.x + b.point.x) / 2, (a.point.y + b.point.y) / 2};
                if (cur.sizePx.x > 0 && cur.sizePx.y > 0)
                    v.scale = std::max(v.width / cur.sizePx.x, v.height / cur.sizePx.y);
            }
            v.layers = ctx.host().setting(kViewLayerSnapshotKey).value_or("1") != "0";
            ctx.setUndoLabel("VIEW Save");
            if (v.layers) {
                if (auto r =
                        layers::saveLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name));
                    !r) {
                    ctx.message(r.error().message);
                    co_return;
                }
            } else if (existing && existing->layers) {
                (void)layers::deleteLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name));
            }
            const std::string text = encodeNamedView(v);
            if (auto r = ctx.tx().modifyHeader([&](Header& h) {
                    if (!key.empty())
                        h.custom.erase(key);
                    h.custom[kViewPrefix + v.name] = text;
                });
                !r)
                ctx.message(r.error().message);
            else
                ctx.message("View \"" + v.name + "\" saved" +
                            (v.layers ? " with a layer snapshot." : "."));
            co_return;
        }
        if (!existing) {
            if (key.empty())
                missing();
            else
                ctx.message("The saved view \"" + name + "\" is damaged.");
            co_return;
        }
        const NamedView v = *existing;
        if (k.keyword == "Delete") {
            ctx.setUndoLabel("VIEW Delete");
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom.erase(key); });
            if (v.layers)
                (void)layers::deleteLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name));
            ctx.message("View \"" + v.name + "\" deleted.");
        } else if (k.keyword == "Rename") {
            auto to = co_await ctx.getString("New name");
            if (!to.ok())
                co_return;
            const std::string newName(cmd::trim(to.text));
            if (newName.empty())
                co_return;
            const std::string other = keyOf(ctx.doc(), newName);
            if (!other.empty() && other != key) {
                ctx.message("A view named \"" + newName + "\" exists.");
                co_return;
            }
            NamedView nv = v;
            nv.name = newName;
            ctx.setUndoLabel("VIEW Rename");
            if (v.layers)
                (void)layers::renameLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name),
                                               viewLayerStateName(newName));
            const std::string text = encodeNamedView(nv);
            (void)ctx.tx().modifyHeader([&](Header& h) {
                h.custom.erase(key);
                h.custom[kViewPrefix + newName] = text;
            });
            ctx.message("View \"" + v.name + "\" is now \"" + newName + "\".");
        } else if (k.keyword == "LAyers") {
            // Save the layers as they are now with the view (or update its snapshot).
            NamedView nv = v;
            nv.layers = true;
            ctx.setUndoLabel("VIEW Layers");
            if (auto r = layers::saveLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name));
                !r) {
                ctx.message(r.error().message);
                co_return;
            }
            const std::string text = encodeNamedView(nv);
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = text; });
            ctx.message("The layer snapshot of \"" + v.name + "\" is updated.");
        } else if (k.keyword == "Category") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto c = co_await ctx.getString("Category (Enter for none)", so);
            if (!c.ok() && c.status != cmd::Status::None)
                co_return;
            NamedView nv = v;
            nv.category = c.ok() ? std::string(cmd::trim(c.text)) : std::string();
            for (char& ch : nv.category)
                if (ch == '\t')
                    ch = ' ';
            ctx.setUndoLabel("VIEW Category");
            const std::string text = encodeNamedView(nv);
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = text; });
        } else {
            restore(ctx, v);
        }
    }

private:
    static void restore(cmd::CommandContext& ctx, const NamedView& v) {
        cmd::IHost& host = ctx.host();
        ctx.setUndoLabel("VIEW Restore");
        const std::string sheet = host.setting("layout.current").value_or("");
        const bool editing = host.setting("tmp.layout.editing").has_value();
        if (!sheet.empty() && !editing) {
            // On the sheet: the view goes into a viewport.
            const auto lay = layout::findLayout(ctx.doc(), sheet);
            const int id = lay ? sheetViewport(host, *lay) : 0;
            const layout::Viewport* vp = id ? lay->findViewport(id) : nullptr;
            if (!vp) {
                ctx.message(
                    "Select a viewport border on the sheet first, or enter one with MSPACE.");
                return;
            }
            if (vp->locked) {
                ctx.message("The viewport is locked (VPLOCK).");
                return;
            }
            if (v.width > 0 && v.height > 0 && vp->paper.width() > 0 && vp->paper.height() > 0)
                (void)layout::setViewportScale(
                    ctx.tx(), lay->name, id,
                    std::max(v.width / vp->paper.width(), v.height / vp->paper.height()));
            (void)layout::setViewportCenter(ctx.tx(), lay->name, id, v.center);
            ctx.message("View \"" + v.name + "\" restored in viewport " + std::to_string(id) + ".");
        } else {
            cmd::ViewState now = host.view();
            cmd::pushViewHistory(host, now);
            cmd::ViewState nv = now;
            nv.center = v.center;
            nv.scale = v.scale;
            // Fit the saved area into this window, which may differ in size from the one it was
            // saved in.
            if (v.width > 0 && v.height > 0 && now.sizePx.x > 0 && now.sizePx.y > 0)
                nv.scale = std::max(v.width / now.sizePx.x, v.height / now.sizePx.y);
            host.setView(nv);
        }
        if (v.layers) {
            layers::StateParts parts;
            parts.lock = parts.plot = parts.look = parts.current = false;
            if (auto r = layers::restoreLayerState(ctx.doc(), ctx.tx(), viewLayerStateName(v.name),
                                                   parts, nullptr);
                !r)
                ctx.message(r.error().message);
        }
    }
};

}  // namespace

std::vector<NamedView> namedViews(const Document& doc) {
    std::vector<NamedView> out;
    for (const auto& [key, value] : doc.header().custom)
        if (key.rfind(kViewPrefix, 0) == 0)
            if (auto v = decode(key.substr(std::string_view(kViewPrefix).size()), value))
                out.push_back(std::move(*v));
    std::sort(out.begin(), out.end(),
              [](const NamedView& a, const NamedView& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::optional<NamedView> findNamedView(const Document& doc, const std::string& name) {
    const std::string key = keyOf(doc, name);
    if (key.empty())
        return std::nullopt;
    return decode(key.substr(std::string_view(kViewPrefix).size()), doc.header().custom.at(key));
}

std::string encodeNamedView(const NamedView& v) {
    std::string s = num(v.center.x) + " " + num(v.center.y) + " " + num(v.scale);
    if (v.width > 0 && v.height > 0)
        s += " " + num(v.width) + " " + num(v.height);
    if (v.layers)
        s += "\tL";
    if (!v.category.empty())
        s += "\tC" + v.category;
    return s;
}

std::string viewLayerStateName(const std::string& view) {
    return "*view|" + view;
}

std::unique_ptr<cmd::Command> makeViewCommand() {
    return std::make_unique<ViewCommand>();
}
std::unique_ptr<cmd::Command> makeViewManagerCommand() {
    return std::make_unique<ViewManagerCommand>();
}

}  // namespace tbcad::sysvars
