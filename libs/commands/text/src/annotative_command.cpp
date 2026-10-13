#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Task;

bool canBeAnnotative(const Entity& e) {
    // A block insert has no live scaling: its own scale is re-set when its annotation scale
    // changes, so symbols (north arrows, markers) can be drawn at 1:1 and sized for 1:N.
    return canScaleAnnotatively(kind(e)) || kind(e) == EntityKind::Insert;
}

// "50,100" -> {50,100}; empty, "current" or "-" -> {} (follows the drawing's current scale).
std::optional<std::vector<double>> parseScales(const std::string& text) {
    std::vector<double> out;
    std::size_t at = 0;
    bool any = false;
    while (at <= text.size()) {
        std::size_t sep = text.find_first_of(", ;", at);
        std::string part = text.substr(at, sep == std::string::npos ? sep : sep - at);
        if (part.rfind("1:", 0) == 0)
            part = part.substr(2);
        if (!part.empty()) {
            any = true;
            char* end = nullptr;
            double v = std::strtod(part.c_str(), &end);
            if (end == part.c_str() || *end != 0 || !(v > 0) || !std::isfinite(v))
                return std::nullopt;
            if (std::find(out.begin(), out.end(), v) == out.end())
                out.push_back(v);
        }
        if (sep == std::string::npos)
            break;
        at = sep + 1;
    }
    (void)any;
    return out;
}

// What an object is drawn at in model space.
double primaryScale(const Header& h, const EntityCommon& c) {
    return !c.annoScales.empty() ? c.annoScales.front() : currentAnnotationScale(h);
}

std::string scaleText(double n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "1:%g", n);
    return buf;
}

// Annotative objects are drawn again at the new scale: a no-op edit marks them as changed.
void retouchAnnotative(CommandContext& ctx) {
    std::vector<EntityId> ids;
    ctx.doc().forEachEntity([&](const Entity& e) {
        if (e.common.annotative)
            ids.push_back(e.common.id);
    });
    for (EntityId id : ids)
        (void)ctx.tx().modify(id, [](Entity&) {});
}

// ANNOTATIVE [ANNO]: turns annotative scaling on or off for the selected text, dimensions,
// leaders, tables and symbols, or sets the scales they are made for.
class AnnotativeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNOTATIVE"; }
    std::vector<std::string> defaultAliases() const override { return {"ANNO"}; }
    std::string_view description() const override {
        return "Make objects follow an annotation scale, or choose their scales";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection(
            "Select text, dimensions, leaders, tables or symbols",
            cmd::SelectionOptions{.filter = [](const Entity& e) { return canBeAnnotative(e); }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off", "Scales"};
        ko.defaultKeyword = "On";
        auto k = co_await ctx.getKeyword("Annotative [On/Off/Scales] <On>", ko);
        if (!k.ok())
            co_return;
        std::optional<std::vector<double>> scales;  // set by "Scales"
        if (k.keyword == "Scales") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto t =
                co_await ctx.getString("Scales as N of 1:N, e.g. 50,100 (empty = current)", so);
            if (!t.ok())
                co_return;
            scales = parseScales(t.text);
            if (!scales) {
                ctx.message("Scales must be numbers above zero, like 50,100.");
                co_return;
            }
        }
        const bool on = k.keyword != "Off";
        const Header header = ctx.doc().header();
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !canBeAnnotative(*e))
                continue;
            const bool isInsert = kind(*e) == EntityKind::Insert;
            const double oldN = e->common.annotative ? primaryScale(header, e->common) : 1.0;
            std::vector<double> next = scales ? *scales : e->common.annoScales.get();
            if (on && isInsert && next.empty())
                next = {currentAnnotationScale(header)};
            const double newN =
                !on ? 1.0 : (next.empty() ? currentAnnotationScale(header) : next.front());
            if (e->common.annotative == on && ScaleList(next) == e->common.annoScales)
                continue;
            auto r = ctx.tx().modify(id, [&](Entity& x) {
                x.common.annotative = on;
                x.common.annoScales = ScaleList(on ? next : std::vector<double>{});
                if (auto* ins = entityAs<InsertData>(x)) {
                    ins->scale = ins->scale * (newN / oldN);
                }
            });
            if (r)
                ++n;
        }
        ctx.message(std::to_string(n) + (on ? " set" : " no longer annotative"));
        ctx.setUndoLabel("Annotative");
    }
};

// ANNONEW: new text, dimensions, leaders, tables and symbols are annotative.
class AnnoNewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNONEW"; }
    std::string_view description() const override {
        return "New annotation objects follow the annotation scale";
    }
    Task run(CommandContext& ctx) override {
        const bool now = annotateNewObjects(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off"};
        ko.defaultKeyword = now ? "Off" : "On";
        auto k = co_await ctx.getKeyword(
            std::string("New objects annotative [On/Off] <") + (now ? "Off" : "On") + ">", ko);
        if (!k.ok())
            co_return;
        const bool on = k.keyword == "On";
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kAnnoNewKey] = on ? "1" : "0"; });
        ctx.message(on ? "New objects are annotative at the current scale."
                       : "New objects are ordinary.");
        ctx.setUndoLabel("Annotative new objects");
    }
};

// CANNOSCALE [AS]: the current annotation scale. Annotative objects are drawn at this scale.
class AnnoScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CANNOSCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"AS"}; }
    std::string_view description() const override { return "Set the annotation scale"; }

    Task run(CommandContext& ctx) override {
        const double cur = currentAnnotationScale(ctx.doc().header());
        cmd::DistanceOptions o;
        auto r = co_await ctx.getDistance(
            "Annotation scale, N of 1:N <" + scaleText(cur).substr(2) + ">", o);
        if (!r.ok())
            co_return;
        if (!(r.value > 0) || !std::isfinite(r.value)) {
            ctx.message("The scale must be above zero.");
            co_return;
        }
        const double v = r.value;
        (void)ctx.tx().modifyHeader([&](Header& h) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.12g", v);
            h.custom[kAnnoScaleKey] = buf;
        });
        retouchAnnotative(ctx);
        ctx.message("Annotation scale: " + scaleText(v));
        ctx.setUndoLabel("Annotation scale");
    }
};

// SCALELISTEDIT: the scales offered for annotation.
class ScaleListCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SCALELISTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"SCALELIST"}; }
    std::string_view description() const override { return "Edit the list of annotation scales"; }

    Task run(CommandContext& ctx) override {
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add", "Delete", "Reset", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Scales [Add/Delete/Reset/List/Done] <Done>", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            std::vector<double> list = annotationScaleList(ctx.doc().header());
            if (k.keyword == "List") {
                std::string s;
                for (double v : list)
                    s += (s.empty() ? "" : "  ") + scaleText(v);
                ctx.message(s);
                continue;
            }
            if (k.keyword == "Reset") {
                (void)ctx.tx().modifyHeader([](Header& h) { h.custom.erase(kAnnoScaleListKey); });
                continue;
            }
            cmd::DistanceOptions o;
            auto r = co_await ctx.getDistance("Scale, N of 1:N", o);
            if (!r.ok())
                co_return;
            const double v = r.value;
            if (!(v > 0) || !std::isfinite(v)) {
                ctx.message("The scale must be above zero.");
                continue;
            }
            auto it = std::find(list.begin(), list.end(), v);
            if (k.keyword == "Add") {
                if (it == list.end())
                    list.push_back(v);
                std::sort(list.begin(), list.end());
            } else if (it != list.end()) {
                list.erase(it);
            } else {
                ctx.message("That scale is not in the list.");
                continue;
            }
            if (list.empty()) {
                ctx.message("The list needs at least one scale.");
                continue;
            }
            std::string joined;
            for (double x : list) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.12g", x);
                joined += (joined.empty() ? "" : ",") + std::string(buf);
            }
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kAnnoScaleListKey] = joined; });
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAnnotativeCommand() {
    return std::make_unique<AnnotativeCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoNewCommand() {
    return std::make_unique<AnnoNewCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoScaleCommand() {
    return std::make_unique<AnnoScaleCommand>();
}
std::unique_ptr<cmd::Command> makeScaleListCommand() {
    return std::make_unique<ScaleListCommand>();
}

}  // namespace tbcad::textcmd
