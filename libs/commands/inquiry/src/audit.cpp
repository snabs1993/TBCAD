#include <cmath>
#include <memory>

#include "extra.h"

namespace tbcad::inquiry {

namespace {

bool finite(Vec2 v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

// Why an object is broken, or an empty string when it is fine.
std::string problemOf(const Document& doc, const Entity& e) {
    if (const auto* l = entityAs<LineData>(e)) {
        if (!finite(l->a) || !finite(l->b))
            return "line with a coordinate that is not a number";
        if (l->a.x == l->b.x && l->a.y == l->b.y)
            return "line of zero length";
    } else if (const auto* c = entityAs<CircleData>(e)) {
        if (!finite(c->center) || !std::isfinite(c->radius))
            return "circle with a value that is not a number";
        if (c->radius <= 0)
            return "circle with no radius";
    } else if (const auto* a = entityAs<ArcData>(e)) {
        if (!finite(a->center) || !std::isfinite(a->radius))
            return "arc with a value that is not a number";
        if (a->radius <= 0)
            return "arc with no radius";
    } else if (const auto* p = entityAs<PolylineData>(e)) {
        if (p->v.empty())
            return "polyline with no vertices";
        for (const auto& v : p->v)
            if (!finite(v.p) || !std::isfinite(v.bulge))
                return "polyline with a coordinate that is not a number";
    } else if (const auto* i = entityAs<InsertData>(e)) {
        if (!finite(i->position))
            return "block reference with a position that is not a number";
        if (!doc.block(i->block))
            return "block reference to a block that does not exist";
    }
    return {};
}

// ---- AUDIT --------------------------------------------------------------------------------------
class Audit final : public cmd::Command {
public:
    std::string_view name() const override { return "AUDIT"; }
    std::string_view description() const override {
        return "Check the drawing for broken objects and offer to remove them";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        std::vector<EntityId> bad;
        std::vector<std::string> why;
        doc.forEachEntity([&](const Entity& e) {
            std::string p = problemOf(doc, e);
            if (!p.empty()) {
                bad.push_back(e.common.id);
                why.push_back(std::move(p));
            }
        });
        ctx.message("Audit checked " + std::to_string(doc.entityCount()) + " objects.");
        if (bad.empty()) {
            ctx.message("No problems found.");
            co_return;
        }
        constexpr std::size_t kShown = 10;
        for (std::size_t i = 0; i < bad.size() && i < kShown; ++i)
            ctx.message("  " + std::to_string(bad[i].value) + ": " + why[i]);
        if (bad.size() > kShown)
            ctx.message("  ... and " + std::to_string(bad.size() - kShown) + " more");
        ctx.message(std::to_string(bad.size()) + " broken object(s) found.");

        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto fix = co_await ctx.getKeyword("Remove the broken objects?", ko);
        if (!fix.ok() || fix.keyword != "Yes")
            co_return;
        std::size_t removed = 0;
        for (EntityId id : bad)
            if (ctx.tx().erase(id))
                ++removed;
        ctx.message("Removed " + std::to_string(removed) + " object(s).");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAudit() {
    return std::make_unique<Audit>();
}

}  // namespace tbcad::inquiry
