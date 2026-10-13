#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "module_state.h"
#include "tbcad/draftingcmds/drafting_cmds.h"

namespace tbcad::draftingcmds {

namespace {

using cmd::KeywordOptions;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string describe(const drafting::Ucs& u) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "origin %.6g,%.6g, rotated %.6g degrees", u.origin.x, u.origin.y,
                  u.angle * 180.0 / kPi);
    return buf;
}

// Direction of the straight piece of `e` nearest to `pick`, with its start. False for shapes that
// have no direction (points, text ...).
bool objectFrame(const Entity& e, Vec2 pick, drafting::Ucs& out) {
    auto fromSegment = [&](Vec2 a, Vec2 b) {
        if (lengthSquared(b - a) < 1e-18)
            return false;
        out.origin = a;
        out.angle = std::atan2(b.y - a.y, b.x - a.x);
        return true;
    };
    if (const auto* l = std::get_if<LineData>(&e.data))
        return fromSegment(l->a, l->b);
    if (const auto* c = std::get_if<CircleData>(&e.data)) {
        out.origin = c->center;  // the axes stay as they are
        return true;
    }
    if (const auto* a = std::get_if<ArcData>(&e.data)) {
        out.origin = a->center;
        return true;
    }
    if (const auto* p = std::get_if<PolylineData>(&e.data)) {
        const std::size_t n = p->v.size();
        if (n < 2)
            return false;
        double best = 1e300;
        std::size_t bi = 0;
        const std::size_t segs = p->closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            Vec2 a = p->v[i].p, b = p->v[(i + 1) % n].p;
            Vec2 ab = b - a;
            double len2 = lengthSquared(ab);
            double t = len2 > 0 ? std::clamp(dot(pick - a, ab) / len2, 0.0, 1.0) : 0.0;
            double d = length(pick - (a + ab * t));
            if (d < best) {
                best = d;
                bi = i;
            }
        }
        return fromSegment(p->v[bi].p, p->v[(bi + 1) % n].p);
    }
    return false;
}

// UCS: the origin and rotation that typed coordinates, ortho / polar tracking and the grid follow.
class UcsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "UCS"; }
    std::string_view description() const override {
        return "Set, save and restore user coordinate systems";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const drafting::Ucs cur = drafting::currentUcs(&ctx.doc(), &ctx.host());
        KeywordOptions menu;
        menu.keywords = {"World", "Origin",  "Rotate", "Object", "Points",
                         "Save",  "Restore", "Delete", "List"};
        menu.defaultKeyword = "List";
        auto choice = co_await ctx.getKeyword("UCS: " +
                                                  (cur.isWorld()      ? std::string("World")
                                                   : cur.name.empty() ? "unnamed"
                                                                      : cur.name) +
                                                  " (" + describe(cur) + ")",
                                              menu);
        if (!choice.ok())
            co_return;
        const std::string& kw = choice.keyword;
        auto apply = [&](drafting::Ucs u) {
            if (u.isWorld())
                ctx.host().setSetting(drafting::kUcsSettingKey, "World");
            else
                ctx.host().setSetting(drafting::kUcsSettingKey,
                                      u.name.empty() ? drafting::unnamedUcsSetting(u) : u.name);
            ctx.message("UCS: " + (u.isWorld() ? std::string("World") : describe(u)));
        };

        if (kw == "World") {
            apply({});
        } else if (kw == "Origin") {
            auto p = co_await ctx.getPoint("Specify the new origin");
            if (!p.ok())
                co_return;
            drafting::Ucs u = cur;
            u.name.clear();
            u.origin = p.point;
            apply(u);
        } else if (kw == "Rotate") {
            cmd::AngleOptions ao;
            auto a = co_await ctx.getAngle("Rotation angle about the origin", ao);
            if (!a.ok())
                co_return;
            drafting::Ucs u = cur;
            u.name.clear();
            u.angle = std::remainder(cur.angle + a.value, kTwoPi);
            apply(u);
        } else if (kw == "Points") {
            auto o = co_await ctx.getPoint("Specify the origin");
            if (!o.ok())
                co_return;
            cmd::PointOptions po;
            po.base = o.point;
            auto x = co_await ctx.getPoint("Specify a point on the X axis", po);
            if (!x.ok())
                co_return;
            if (lengthSquared(x.point - o.point) < 1e-18) {
                ctx.message("The X axis point is the same as the origin.");
                co_return;
            }
            drafting::Ucs u;
            u.origin = o.point;
            u.angle = std::atan2(x.point.y - o.point.y, x.point.x - o.point.x);
            apply(u);
        } else if (kw == "Object") {
            auto e = co_await ctx.getEntity("Select a line, arc, circle or polyline");
            if (!e.ok())
                co_return;
            const Entity* ent = ctx.doc().find(e.id);
            drafting::Ucs u = cur;
            u.name.clear();
            if (!ent || !objectFrame(*ent, e.pickPoint, u)) {
                ctx.message("That object has no direction to align to.");
                co_return;
            }
            apply(u);
        } else if (kw == "Save") {
            cmd::StringOptions so;
            auto n = co_await ctx.getString("Name for the current UCS", so);
            if (!n.ok())
                co_return;
            drafting::Ucs u = cur;
            u.name = n.text;
            auto r = drafting::saveUcs(ctx.tx(), u);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            apply(u);
        } else if (kw == "Restore") {
            cmd::StringOptions so;
            auto n =
                co_await ctx.getString("Name of the UCS to restore (World for the default)", so);
            if (!n.ok())
                co_return;
            if (lower(n.text) == "world") {
                apply({});
                co_return;
            }
            for (const auto& u : drafting::ucsList(ctx.doc())) {
                if (lower(u.name) == lower(n.text)) {
                    apply(u);
                    co_return;
                }
            }
            ctx.message("No UCS named \"" + n.text + "\".");
        } else if (kw == "Delete") {
            cmd::StringOptions so;
            auto n = co_await ctx.getString("Name of the UCS to delete", so);
            if (!n.ok())
                co_return;
            auto r = drafting::deleteUcs(ctx.tx(), n.text);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            if (lower(cur.name) == lower(n.text))
                ctx.host().setSetting(drafting::kUcsSettingKey, "World");
            ctx.message("Deleted UCS \"" + n.text + "\".");
        } else {  // List
            auto all = drafting::ucsList(ctx.doc());
            if (all.empty()) {
                ctx.message("No saved UCS. Use UCS > Save.");
                co_return;
            }
            for (const auto& u : all)
                ctx.message((lower(u.name) == lower(cur.name) ? "* " : "  ") + u.name + "  " +
                            describe(u));
        }
    }
};

}  // namespace

void registerUcsCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<UcsCommand>());
}

}  // namespace tbcad::draftingcmds
