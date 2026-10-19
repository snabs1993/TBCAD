#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/mline/mline.h"
#include "tbcad/mlinecmd/mlinecmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::mlinecmd {

namespace {

constexpr const char* kStyleKey = "mline.style";
constexpr const char* kScaleKey = "mline.scale";
constexpr const char* kJustKey = "mline.just";

std::string currentStyle(cmd::CommandContext& ctx) {
    return ctx.host().setting(kStyleKey).value_or("STANDARD");
}
double currentScale(cmd::CommandContext& ctx) {
    auto s = ctx.host().setting(kScaleKey);
    if (s) {
        char* end = nullptr;
        double v = std::strtod(s->c_str(), &end);
        if (end != s->c_str() && v > 0)
            return v;
    }
    return 1.0;
}
int currentJust(cmd::CommandContext& ctx) {
    auto s = ctx.host().setting(kJustKey);
    if (s && (*s == "0" || *s == "1" || *s == "2"))
        return (*s)[0] - '0';
    return 1;
}

const char* justName(int j) {
    return j == 0 ? "Top" : (j == 2 ? "Bottom" : "Zero");
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::vector<double>> parseOffsets(const std::string& text) {
    std::vector<double> out;
    std::string t = text;
    for (char& c : t)
        if (c == ',' || c == ';')
            c = ' ';
    std::istringstream in(t);
    std::string tok;
    while (in >> tok) {
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0' || !std::isfinite(v))
            return std::nullopt;
        out.push_back(v);
    }
    if (out.empty() || out.size() > 16)
        return std::nullopt;
    return out;
}

void previewMline(cmd::CommandContext& ctx, const MlineData& d, DisplayList& out) {
    Entity e;
    e.common.layer = ctx.doc().header().currentLayer;
    e.data = Indirect<MlineData>(d);
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
        // a half-defined preview simply shows nothing
    }
}

// ---- MLINE -------------------------------------------------------------------------------------

class MlineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"ML"}; }
    std::string_view description() const override {
        return "Draw several parallel lines at once (walls)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        MlineData d;
        d.style = currentStyle(ctx);
        d.scale = currentScale(ctx);
        d.justification = currentJust(ctx);
        if (!mline::findStyle(ctx.doc(), d.style))
            d.style = "STANDARD";

        // Options before the first point.
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Justification", "Scale", "STyle"};
            auto first = co_await ctx.getPoint(
                "Current: " + std::string(justName(d.justification)) + ", scale " +
                    std::to_string(d.scale) + ", style " + d.style +
                    ". Specify start point or [Justification/Scale/STyle]",
                po);
            if (first.status == Status::Keyword) {
                if (first.keyword == "Justification") {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"Top", "Zero", "Bottom"};
                    ko.defaultKeyword = justName(d.justification);
                    auto j = co_await ctx.getKeyword("Justification [Top/Zero/Bottom]", ko);
                    if (!j.ok())
                        co_return;
                    d.justification = j.keyword == "Top" ? 0 : (j.keyword == "Bottom" ? 2 : 1);
                    ctx.host().setSetting(kJustKey, std::to_string(d.justification));
                } else if (first.keyword == "Scale") {
                    cmd::RealOptions ro;
                    ro.defaultValue = d.scale;
                    ro.min = 1e-6;
                    auto s = co_await ctx.getReal("Multiline scale", ro);
                    if (!s.ok())
                        co_return;
                    d.scale = s.value;
                    ctx.host().setSetting(kScaleKey, std::to_string(d.scale));
                } else {
                    ctx.message("Styles: " + joinNames(ctx));
                    cmd::StringOptions so;
                    so.defaultValue = d.style;
                    auto n = co_await ctx.getString("Style name", so);
                    if (!n.ok())
                        co_return;
                    auto found = mline::findStyle(ctx.doc(), cmd::trim(n.text));
                    if (!found) {
                        ctx.message("No style named \"" + cmd::trim(n.text) + "\".");
                        continue;
                    }
                    d.style = found->name;
                    ctx.host().setSetting(kStyleKey, d.style);
                }
                continue;
            }
            if (!first.ok())
                co_return;
            d.v.push_back(first.point);
            break;
        }

        ctx.commitOnCancel();
        auto* c = &ctx;
        for (;;) {
            cmd::PointOptions po;
            po.base = d.v.back();
            if (d.v.size() >= 2)
                po.keywords = {"Close", "Undo"};
            else
                po.keywords = {"Undo"};
            po.allowNone = true;
            po.preview = [c, d](Vec2 cursor, DisplayList& out) {
                MlineData t = d;
                t.v.push_back(cursor);
                previewMline(*c, t, out);
            };
            auto next = co_await ctx.getPoint(d.v.size() >= 2 ? "Specify next point or [Close/Undo]"
                                                              : "Specify next point or [Undo]",
                                              po);
            if (next.status == Status::Keyword) {
                if (next.keyword == "Close") {
                    d.closed = true;
                    break;
                }
                if (d.v.size() > 1)
                    d.v.pop_back();
                continue;
            }
            if (next.status == Status::None || next.status == Status::Cancel)
                break;
            if (!next.ok())
                break;
            if (distance(d.v.back(), next.point) <= 1e-9)
                continue;
            d.v.push_back(next.point);
        }
        if (d.v.size() < 2)
            co_return;
        ctx.setUndoLabel("MLINE");
        Entity e;
        e.data = Indirect<MlineData>(std::move(d));
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
    }

private:
    static std::string joinNames(cmd::CommandContext& ctx) {
        std::string all;
        for (const std::string& n : mline::styleNames(ctx.doc()))
            all += (all.empty() ? "" : ", ") + n;
        return all;
    }
};

// ---- MLSTYLE -----------------------------------------------------------------------------------

class MlstyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLSTYLE"; }
    std::string_view description() const override {
        return "Define multiline styles: how many lines, their offsets and end caps";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::KeywordOptions ko;
        ko.keywords = {"List",      "New",  "Delete", "Offsets",     "Caps",    "Colors",
                       "Linetypes", "Fill", "Joints", "Description", "Current", "Arcs"};
        ko.defaultKeyword = "List";
        auto k = co_await ctx.getKeyword(
            "Style option "
            "[List/New/Delete/Offsets/Caps/Arcs/Colors/Linetypes/Fill/Joints/Description/"
            "Current]",
            ko);
        if (!k.ok())
            co_return;

        if (k.keyword == "List") {
            const std::string cur = currentStyle(ctx);
            for (const std::string& n : mline::styleNames(ctx.doc())) {
                auto s = mline::findStyle(ctx.doc(), n);
                std::string line = n + (lower(n) == lower(cur) ? " (current)" : "") + ": offsets";
                if (s)
                    for (double o : s->offsets)
                        line += " " + fmt(o);
                if (s && s->startCap)
                    line += ", start cap";
                if (s && s->endCap)
                    line += ", end cap";
                if (s && s->startArc)
                    line += s->startArc == 1 ? ", outer arc at start" : ", inner arcs at start";
                if (s && s->endArc)
                    line += s->endArc == 1 ? ", outer arc at end" : ", inner arcs at end";
                if (s && s->joints)
                    line += ", joints";
                if (s && s->fill >= 0)
                    line += ", fill " + std::to_string(s->fill);
                if (s && !s->colors.empty()) {
                    line += ", colors";
                    for (int c : s->colors)
                        line += " " + std::to_string(c);
                }
                if (s && !s->linetypes.empty()) {
                    line += ", linetypes";
                    for (const std::string& l : s->linetypes)
                        line += " " + (l.empty() ? std::string("-") : l);
                }
                ctx.message(line);
            }
            co_return;
        }

        cmd::StringOptions so;
        auto n = co_await ctx.getString("Style name", so);
        if (!n.ok())
            co_return;
        const std::string name = cmd::trim(n.text);
        if (name.empty() || name.size() > 64) {
            ctx.message("Give a style name of 1 to 64 characters.");
            co_return;
        }
        auto existing = mline::findStyle(ctx.doc(), name);

        if (k.keyword == "Current") {
            if (!existing) {
                ctx.message("No style named \"" + name + "\".");
                co_return;
            }
            ctx.host().setSetting(kStyleKey, existing->name);
            ctx.message("Current style is " + existing->name + ".");
            co_return;
        }
        if (k.keyword == "Delete") {
            if (!existing || lower(name) == "standard") {
                ctx.message(existing ? "STANDARD cannot be deleted."
                                     : "No style named \"" + name + "\".");
                co_return;
            }
            bool used = false;
            ctx.doc().forEachEntity([&](const Entity& e) {
                if (const auto* m = entityAs<MlineData>(e))
                    if (lower(m->style) == lower(name))
                        used = true;
            });
            if (used) {
                ctx.message("That style is used by a multiline.");
                co_return;
            }
            ctx.setUndoLabel("MLSTYLE");
            (void)ctx.tx().modifyHeader([&](Header& h) { mline::removeStyle(h, name); });
            if (lower(currentStyle(ctx)) == lower(name))
                ctx.host().setSetting(kStyleKey, "STANDARD");
            co_return;
        }

        mline::Style style = existing.value_or(mline::standardStyle());
        if (k.keyword == "New") {
            if (existing) {
                ctx.message("A style named \"" + existing->name + "\" exists already.");
                co_return;
            }
            style.name = name;
            style.description.clear();
        } else if (!existing) {
            ctx.message("No style named \"" + name + "\".");
            co_return;
        }
        if (k.keyword == "New" || k.keyword == "Offsets") {
            std::string def;
            for (double o : style.offsets)
                def += (def.empty() ? "" : " ") + fmt(o);
            cmd::StringOptions oo;
            oo.defaultValue = def;
            auto t =
                co_await ctx.getString("Line offsets, left of the path positive <" + def + ">", oo);
            if (!t.ok())
                co_return;
            auto offs = parseOffsets(t.text);
            if (!offs) {
                ctx.message("Offsets are numbers separated by spaces or commas.");
                co_return;
            }
            if (offs->size() != style.offsets.size()) {  // per-line colours/linetypes no longer fit
                style.colors.clear();
                style.linetypes.clear();
            }
            style.offsets = *offs;
        }
        if (k.keyword == "New" || k.keyword == "Caps") {
            cmd::KeywordOptions co;
            co.keywords = {"None", "Start", "End", "Both"};
            co.defaultKeyword = "None";
            auto c = co_await ctx.getKeyword("End caps [None/Start/End/Both]", co);
            if (!c.ok())
                co_return;
            style.startCap = c.keyword == "Start" || c.keyword == "Both";
            style.endCap = c.keyword == "End" || c.keyword == "Both";
        }
        if (k.keyword == "Arcs") {
            cmd::KeywordOptions ao;
            ao.keywords = {"None", "Outer", "Inner"};
            ao.defaultKeyword = "None";
            auto a1 = co_await ctx.getKeyword("Arcs at the start [None/Outer/Inner]", ao);
            if (!a1.ok())
                co_return;
            auto a2 = co_await ctx.getKeyword("Arcs at the end [None/Outer/Inner]", ao);
            if (!a2.ok())
                co_return;
            style.startArc = a1.keyword == "Outer" ? 1 : a1.keyword == "Inner" ? 2 : 0;
            style.endArc = a2.keyword == "Outer" ? 1 : a2.keyword == "Inner" ? 2 : 0;
        } else if (k.keyword == "Colors") {
            std::string def;
            for (std::size_t i = 0; i < style.offsets.size(); ++i)
                def += (def.empty() ? "" : " ") + std::to_string(style.colorOf(i));
            cmd::StringOptions oo;
            oo.defaultValue = def;
            auto t = co_await ctx.getString(
                "Colors, one per line in offset order (1-255, 0 ByBlock, 256 as the multiline) <" +
                    def + ">",
                oo);
            if (!t.ok())
                co_return;
            auto vals = parseOffsets(t.text);
            if (!vals || vals->size() != style.offsets.size()) {
                ctx.message("Give one colour number for each of the " +
                            std::to_string(style.offsets.size()) + " lines.");
                co_return;
            }
            style.colors.clear();
            for (double v : *vals) {
                if (v < 0 || v > 256 || v != std::floor(v)) {
                    ctx.message("Colours are whole numbers from 0 to 256.");
                    co_return;
                }
                style.colors.push_back(static_cast<int>(v));
            }
            if (std::all_of(style.colors.begin(), style.colors.end(),
                            [](int c) { return c == 256; }))
                style.colors.clear();
        } else if (k.keyword == "Linetypes") {
            cmd::StringOptions oo;
            auto t = co_await ctx.getString(
                "Linetype names, one per line in offset order, - for the multiline's own", oo);
            if (!t.ok())
                co_return;
            std::vector<std::string> names;
            std::istringstream in(t.text);
            for (std::string w; in >> w;)
                names.push_back(w == "-" ? std::string() : w);
            if (names.size() != style.offsets.size()) {
                ctx.message("Give one linetype for each of the " +
                            std::to_string(style.offsets.size()) + " lines.");
                co_return;
            }
            for (const std::string& n : names)
                if (!n.empty() && !ctx.doc().linetypeByName(n)) {
                    ctx.message("No linetype named \"" + n + "\" is loaded.");
                    co_return;
                }
            style.linetypes = std::all_of(names.begin(), names.end(),
                                          [](const std::string& n) { return n.empty(); })
                                  ? std::vector<std::string>{}
                                  : names;
        } else if (k.keyword == "Fill") {
            cmd::StringOptions oo;
            oo.defaultValue = "None";
            auto t =
                co_await ctx.getString("Fill colour (1-255, 256 as the multiline) or None", oo);
            if (!t.ok())
                co_return;
            const std::string v = lower(cmd::trim(t.text));
            if (v == "none" || v.empty()) {
                style.fill = -1;
            } else {
                auto vals = parseOffsets(v);
                if (!vals || vals->size() != 1 || (*vals)[0] < 0 || (*vals)[0] > 256) {
                    ctx.message("Give a colour number from 0 to 256, or None.");
                    co_return;
                }
                style.fill = static_cast<int>((*vals)[0]);
            }
        } else if (k.keyword == "Joints") {
            cmd::KeywordOptions jo;
            jo.keywords = {"On", "Off"};
            jo.defaultKeyword = style.joints ? "Off" : "On";
            auto j = co_await ctx.getKeyword("Show joints at the corners [On/Off]", jo);
            if (!j.ok())
                co_return;
            style.joints = j.keyword == "On";
        } else if (k.keyword == "Description") {
            cmd::StringOptions oo;
            oo.defaultValue = style.description;
            auto t = co_await ctx.getString("Description", oo);
            if (!t.ok())
                co_return;
            style.description = cmd::trim(t.text);
            for (char& c : style.description)
                if (c == '\n' || c == '\r')
                    c = ' ';
        }
        ctx.setUndoLabel("MLSTYLE");
        (void)ctx.tx().modifyHeader([&](Header& h) { mline::storeStyle(h, style); });
        ctx.message("Style " + style.name + " saved.");
    }

private:
    static std::string fmt(double v) {
        std::ostringstream o;
        o << v;
        return o.str();
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMlEditCommand();

void registerCommands(cmd::Registry& registry) {
    mline::registerDisplay();
    blocks::ExplodeRegistry::set(
        EntityKind::MLine,
        [](const Document& doc, const Entity& e) -> Result<blocks::ExplodeResult> {
            auto r = mline::explodeMline(doc, e);
            if (!r)
                return r.error();
            return blocks::ExplodeResult{std::move(r).value(), {}};
        });
    (void)registry.add(std::make_unique<MlineCommand>());
    (void)registry.add(std::make_unique<MlstyleCommand>());
    (void)registry.add(makeMlEditCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Multiline", "MLINE");
    ui.addMenuItem("Modify/Edit Multiline", "MLEDIT");
    ui.addMenuItem("Format/Multiline Style", "MLSTYLE");
}

}  // namespace tbcad::mlinecmd
