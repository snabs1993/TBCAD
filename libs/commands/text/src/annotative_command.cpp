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
    switch (kind(e)) {
        case EntityKind::Text:
        case EntityKind::MText:
        case EntityKind::Dimension:
        case EntityKind::Leader:
        case EntityKind::Table:
            return true;
        default:
            return false;
    }
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
// leaders and tables.
class AnnotativeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNOTATIVE"; }
    std::vector<std::string> defaultAliases() const override { return {"ANNO"}; }
    std::string_view description() const override {
        return "Make text, dimensions and tables follow the annotation scale";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection(
            "Select text, dimensions, leaders or tables",
            cmd::SelectionOptions{.filter = [](const Entity& e) { return canBeAnnotative(e); }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off"};
        ko.defaultKeyword = "On";
        auto k = co_await ctx.getKeyword("Annotative [On/Off] <On>", ko);
        if (!k.ok())
            co_return;
        const bool on = k.keyword != "Off";
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !canBeAnnotative(*e) || e->common.annotative == on)
                continue;
            if (ctx.tx().modify(id, [on](Entity& x) { x.common.annotative = on; }))
                ++n;
        }
        ctx.message(std::to_string(n) + (on ? " made annotative" : " no longer annotative"));
        ctx.setUndoLabel("Annotative");
    }
};

// "50", "1:50" and "1/50" all mean one drawing unit to 50; anything else is rejected.
std::optional<double> parseScaleN(const std::string& text) {
    std::string t;
    for (char c : text)
        if (c != ' ')
            t += c;
    for (const char* prefix : {"1:", "1/"})
        if (t.rfind(prefix, 0) == 0) {
            t.erase(0, 2);
            break;
        }
    if (t.empty())
        return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

// CANNOSCALE [AS]: the current annotation scale. Annotative objects are drawn at this scale.
class AnnoScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CANNOSCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"AS"}; }
    std::string_view description() const override { return "Set the annotation scale"; }

    Task run(CommandContext& ctx) override {
        const double cur = currentAnnotationScale(ctx.doc().header());
        const std::string def = scaleText(cur).substr(2);
        cmd::StringOptions so;
        so.defaultValue = def;
        auto r = co_await ctx.getString("Annotation scale, N or 1:N <" + def + ">", so);
        if (!r.ok())
            co_return;
        const auto parsed = parseScaleN(r.text.empty() ? def : r.text);
        if (!parsed || !(*parsed > 0)) {
            ctx.message("Enter the scale as N or 1:N, a number above zero.");
            co_return;
        }
        const double v = *parsed;
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
            auto r = co_await ctx.getString("Scale, N or 1:N");
            if (!r.ok())
                co_return;
            const auto parsed = parseScaleN(r.text);
            if (!parsed || !(*parsed > 0)) {
                ctx.message("Enter the scale as N or 1:N, a number above zero.");
                continue;
            }
            const double v = *parsed;
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
std::unique_ptr<cmd::Command> makeAnnoScaleCommand() {
    return std::make_unique<AnnoScaleCommand>();
}
std::unique_ptr<cmd::Command> makeScaleListCommand() {
    return std::make_unique<ScaleListCommand>();
}

}  // namespace tbcad::textcmd
