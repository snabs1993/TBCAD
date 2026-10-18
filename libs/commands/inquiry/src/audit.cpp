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
    } else if (const auto* el = entityAs<EllipseData>(e)) {
        if (!finite(el->center) || !finite(el->majorAxis) || !std::isfinite(el->ratio))
            return "ellipse with a value that is not a number";
        if (el->majorAxis.x == 0 && el->majorAxis.y == 0)
            return "ellipse with no size";
    } else if (const auto* t = entityAs<TextData>(e)) {
        if (!finite(t->position) || !std::isfinite(t->height))
            return "text with a value that is not a number";
        if (t->height <= 0)
            return "text with no height";
    } else if (const auto* h = entityAs<HatchData>(e)) {
        if (h->loops.empty())
            return "hatch with no boundary";
    } else if (const auto* i = entityAs<InsertData>(e)) {
        if (!finite(i->position))
            return "block reference with a position that is not a number";
        if (!doc.block(i->block))
            return "block reference to a block that does not exist";
    }
    return {};
}

// ---- AUDIT --------------------------------------------------------------------------------------
// RECOVER is AUDIT that does not ask: it repairs what can be repaired and removes the rest.
class Audit final : public cmd::Command {
public:
    explicit Audit(bool recover = false) : recover_(recover) {}
    std::string_view name() const override { return recover_ ? "RECOVER" : "AUDIT"; }
    std::string_view description() const override {
        return recover_ ? "Repair the drawing: fix missing layers and remove broken objects"
                        : "Check the drawing for broken objects and offer to remove them";
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
        // Objects on a layer that no longer exists go to layer 0.
        std::vector<EntityId> orphans;
        doc.forEachEntity([&](const Entity& e) {
            if (!doc.layer(e.common.layer))
                orphans.push_back(e.common.id);
        });
        ctx.message("Audit checked " + std::to_string(doc.entityCount()) + " objects.");
        if (!orphans.empty()) {
            ctx.message(std::to_string(orphans.size()) +
                        " object(s) are on a layer that does not exist.");
            cmd::KeywordOptions lo;
            lo.keywords = {"Yes", "No"};
            lo.defaultKeyword = recover_ ? "Yes" : "No";
            std::string answer = "Yes";
            if (!recover_) {
                auto a = co_await ctx.getKeyword("Move them to layer 0?", lo);
                if (!a.ok())
                    co_return;
                answer = a.keyword;
            }
            if (answer == "Yes") {
                for (EntityId id : orphans)
                    (void)ctx.tx().modify(id, [](Entity& e) { e.common.layer = kLayerZero; });
                ctx.message("Moved " + std::to_string(orphans.size()) + " object(s) to layer 0.");
            }
        }
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
        if (!recover_) {
            auto fix = co_await ctx.getKeyword("Remove the broken objects?", ko);
            if (!fix.ok() || fix.keyword != "Yes")
                co_return;
        }
        std::size_t removed = 0;
        for (EntityId id : bad)
            if (ctx.tx().erase(id))
                ++removed;
        ctx.message("Removed " + std::to_string(removed) + " object(s).");
        co_return;
    }

private:
    bool recover_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeAudit() {
    return std::make_unique<Audit>();
}

std::unique_ptr<cmd::Command> makeRecover() {
    return std::make_unique<Audit>(true);
}

}  // namespace tbcad::inquiry
