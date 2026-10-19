#include <cmath>

#include "common.h"

namespace tbcad::select::detail {

namespace {

// A layer by name; "Parent/Child" walks the sub-layer path.
const Layer* findLayer(const Document& doc, const std::string& path) {
    const Layer* cur = nullptr;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? slash : slash - start);
        cur = cur ? doc.layerByName(cur->id, part) : doc.layerByName(part);
        if (!cur || slash == std::string::npos)
            return cur;
        start = slash + 1;
    }
}

// Asks which property to change and its new value, then applies it to `ids`.
cmd::Task changeProperties(cmd::CommandContext& ctx, std::vector<EntityId> ids) {
    for (;;) {
        cmd::KeywordOptions ko;
        ko.keywords = {"Layer", "Color", "Linetype", "Lineweight", "Scale", "Transparency", "Plotstyle"};
        auto k = co_await ctx.getKeyword(
            "Property to change [Layer/Color/Linetype/Lineweight/Scale/Transparency/Plotstyle]", std::move(ko));
        if (!k.ok())
            co_return;
        auto v = co_await ctx.getString("New " + k.keyword + " value");
        if (!v.ok())
            co_return;
        std::function<void(EntityCommon&)> apply;
        if (k.keyword == "Layer") {
            const Layer* l = findLayer(ctx.doc(), v.text);
            if (!l) {
                ctx.message("There is no layer named \"" + v.text + "\".");
                continue;
            }
            const LayerId id = l->id;
            apply = [id](EntityCommon& c) { c.layer = id; };
        } else if (k.keyword == "Color") {
            auto c = parseColor(v.text);
            if (!c) {
                ctx.message("\"" + v.text + "\" is not a colour.");
                continue;
            }
            const Color col = *c;
            apply = [col](EntityCommon& x) { x.color = col; };
        } else if (k.keyword == "Linetype") {
            const std::string u = cmd::toUpperAscii(v.text);
            LinetypeId id = kLinetypeByLayer;
            if (u == "BYBLOCK") {
                id = kLinetypeByBlock;
            } else if (u != "BYLAYER") {
                const Linetype* lt = ctx.doc().linetypeByName(v.text);
                if (!lt) {
                    ctx.message("There is no linetype named \"" + v.text + "\".");
                    continue;
                }
                id = lt->id;
            }
            apply = [id](EntityCommon& x) { x.linetype = id; };
        } else if (k.keyword == "Lineweight") {
            auto w = parseWeight(v.text);
            if (!w) {
                ctx.message("\"" + v.text + "\" is not a lineweight.");
                continue;
            }
            const LineWeight lw = *w;
            apply = [lw](EntityCommon& x) { x.lineweight = lw; };
        } else if (k.keyword == "Transparency") {
            const std::string u = cmd::toUpperAscii(cmd::trim(v.text));
            int t = kTransparencyByLayer;
            if (u == "BYBLOCK") {
                t = kTransparencyByBlock;
            } else if (u != "BYLAYER") {
                try {
                    std::size_t used = 0;
                    t = std::stoi(u, &used);
                    if (used != u.size())
                        t = -99;
                } catch (...) {
                    t = -99;
                }
            }
            if (!validObjectTransparency(t)) {
                ctx.message("The transparency must be 0 to 90 percent, ByLayer or ByBlock.");
                continue;
            }
            const auto value = static_cast<std::int8_t>(t);
            apply = [value](EntityCommon& x) { x.transparency = value; };
        } else if (k.keyword == "Plotstyle") {
            ctx.checkpoint();
            const std::string name = cmd::trim(v.text);
            int n = 0;
            for (EntityId id : ids)
                if (const Entity* e = ctx.doc().find(id); e && !ctx.doc().effective(e->common).locked)
                    ++n;
            (void)ctx.tx().modifyHeader([&](Header& h) {
                for (EntityId id : ids)
                    if (const Entity* e = ctx.doc().find(id);
                        e && !ctx.doc().effective(e->common).locked)
                        setEntityPlotStyle(h, id, name);
            });
            ctx.message(std::to_string(n) + " object(s) changed.");
            co_return;
        } else {
            double scale = 0;
            try {
                std::size_t used = 0;
                scale = std::stod(v.text, &used);
                if (used != v.text.size())
                    scale = 0;
            } catch (...) {
            }
            if (!(scale > 0)) {
                ctx.message("The linetype scale must be a positive number.");
                continue;
            }
            apply = [scale](EntityCommon& x) { x.linetypeScale = static_cast<float>(scale); };
        }
        ctx.checkpoint();
        int n = 0;
        for (EntityId id : ids) {
            const Entity* cur = ctx.doc().find(id);
            if (!cur || ctx.doc().effective(cur->common).locked)
                continue;  // objects on locked layers cannot be edited
            if (ctx.tx().modify(id, [&](Entity& e) { apply(e.common); }))
                ++n;
        }
        ctx.message(std::to_string(n) + " object(s) changed.");
        co_return;
    }
}

// CHPROP [CH]: change the layer, colour, linetype, lineweight or linetype scale of the selected
// objects. Works on the current selection without asking, so panels can run it as a command line
// ("CHPROP Color 1").
class ChProp final : public cmd::Command {
public:
    std::string_view name() const override { return "CHPROP"; }
    std::vector<std::string> defaultAliases() const override { return {"CH"}; }
    std::string_view description() const override {
        return "Change the layer, colour, linetype or lineweight of objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        co_await changeProperties(ctx, sel.ids);
    }
};

// CHANGE [-CH]: moves an end or a size of the selected objects to a point, or changes properties.
// Lines: the end nearest to the point moves there. Circles: the radius reaches the point. Text and
// inserts: the position moves to the point. Arcs and others are left alone.
class Change final : public cmd::Command {
public:
    std::string_view name() const override { return "CHANGE"; }
    std::string_view description() const override {
        return "Move line ends, set radii or positions to a point, or change properties";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::PointOptions po;
        po.keywords = {"Properties"};
        auto pt = co_await ctx.getPoint("Specify change point or [Properties]", std::move(po));
        if (pt.status == cmd::Status::Keyword) {
            co_await changeProperties(ctx, sel.ids);
            co_return;
        }
        if (!pt.ok())
            co_return;
        const Vec2 at = pt.point;
        ctx.setUndoLabel("CHANGE");
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            bool changed = false;
            const Entity* e = ctx.doc().find(id);
            if (!e || ctx.doc().effective(e->common).locked)
                continue;
            if (auto r = ctx.tx().modify(
                    id,
                    [&](Entity& en) {
                        if (auto* l = entityAs<LineData>(en)) {
                            (distance(l->a, at) <= distance(l->b, at) ? l->a : l->b) = at;
                            changed = true;
                        } else if (auto* c = entityAs<CircleData>(en)) {
                            const double r2 = distance(c->center, at);
                            if (r2 > 0) {
                                c->radius = r2;
                                changed = true;
                            }
                        } else if (auto* t = entityAs<TextData>(en)) {
                            const Vec2 d = at - t->position;
                            t->position += d;
                            t->alignPoint += d;
                            changed = true;
                        } else if (auto* ins = entityAs<InsertData>(en)) {
                            ins->position = at;
                            changed = true;
                        }
                    });
                r && changed)
                ++n;
        }
        ctx.message(std::to_string(n) + " object(s) changed.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeChange() {
    return std::make_unique<Change>();
}

std::unique_ptr<cmd::Command> makeChProp() {
    return std::make_unique<ChProp>();
}

}  // namespace tbcad::select::detail
