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
        ko.keywords = {"List", "New", "Delete", "Offsets", "Caps", "Current"};
        ko.defaultKeyword = "List";
        auto k = co_await ctx.getKeyword("Style option [List/New/Delete/Offsets/Caps/Current]", ko);
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
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Multiline", "MLINE");
    ui.addMenuItem("Format/Multiline Style", "MLSTYLE");
}

}  // namespace tbcad::mlinecmd
