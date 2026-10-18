#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

constexpr const char* kRadiusKey = "fillet.radius";
constexpr const char* kFilletTrimKey = "fillet.trim";
constexpr const char* kD1Key = "chamfer.distance1";
constexpr const char* kD2Key = "chamfer.distance2";
constexpr const char* kAngleKey = "chamfer.angle";
constexpr const char* kByAngleKey = "chamfer.byangle";
constexpr const char* kChamferTrimKey = "chamfer.trim";

bool filletable(const Document& doc, const Entity& e) {
    if (!isEditable(doc, e))
        return false;
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Circle:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
        case EntityKind::Polyline:
            return true;
        default:
            return false;
    }
}

bool chamferable(const Document& doc, const Entity& e) {
    if (!isEditable(doc, e))
        return false;
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Circle:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
        case EntityKind::Polyline:
            return true;
        default:
            return false;
    }
}

// Applies a two-entity edit.
void applyPair(cmd::CommandContext& ctx, EntityId a, EntityId b, modify::PairEdit edit) {
    if (edit.modifiedA) {
        if (auto r = replaceEntity(ctx.tx(), a, *edit.modifiedA); !r)
            ctx.message(r.error().message);
    }
    if (edit.modifiedB) {
        if (auto r = replaceEntity(ctx.tx(), b, *edit.modifiedB); !r)
            ctx.message(r.error().message);
    }
    for (Entity& e : edit.added) {
        if (auto r = ctx.tx().add(std::move(e)); !r)
            ctx.message(r.error().message);
    }
}

enum class Corner { Fillet, Chamfer };

// One corner operation for FILLET and CHAMFER: two objects (or two segments of one polyline).
void cornerOp(cmd::CommandContext& ctx, Corner what, const cmd::EntityResult& first,
              const cmd::EntityResult& second) {
    const Entity* a = ctx.doc().find(first.id);
    const Entity* b = ctx.doc().find(second.id);
    if (!a || !b)
        return;
    const bool fillet = what == Corner::Fillet;
    const double radius = second.shift ? 0.0 : getSetting(ctx.host(), kRadiusKey, 0.0);
    modify::ChamferOptions co;
    co.distance1 = getSetting(ctx.host(), kD1Key, 1.0);
    co.distance2 = getSetting(ctx.host(), kD2Key, 1.0);
    co.angle = getSetting(ctx.host(), kAngleKey, kPi / 4);
    co.byAngle = getFlag(ctx.host(), kByAngleKey, false);
    co.trim = getFlag(ctx.host(), kChamferTrimKey, true);
    if (second.shift && !fillet)
        co = modify::ChamferOptions{0, 0, false, 0, true};  // Shift: a sharp corner

    if (first.id == second.id) {
        const auto* pl = entityAs<PolylineData>(*a);
        if (!pl) {
            ctx.message("Select two different objects.");
            return;
        }
        auto v = modify::sharedVertex(*pl, modify::nearestSegmentIndex(*pl, first.pickPoint),
                                      modify::nearestSegmentIndex(*pl, second.pickPoint));
        if (!v) {
            ctx.message("Those two segments do not share a corner.");
            return;
        }
        auto res = fillet ? modify::filletPolylineVertex(*a, *v, radius)
                          : modify::chamferPolylineVertex(*a, *v, co);
        if (!res) {
            ctx.message(res.error().message);
            return;
        }
        if (auto r = replaceEntity(ctx.tx(), first.id, res.value()); !r)
            ctx.message(r.error().message);
        return;
    }
    modify::FilletOptions fo;
    fo.radius = radius;
    fo.trim = getFlag(ctx.host(), kFilletTrimKey, true);
    auto res = fillet ? modify::filletEntities(*a, first.pickPoint, *b, second.pickPoint, fo)
                      : modify::chamferEntities(*a, first.pickPoint, *b, second.pickPoint, co);
    if (!res) {
        ctx.message(res.error().message);
        return;
    }
    applyPair(ctx, first.id, second.id, std::move(res.value()));
}

void polylineOp(cmd::CommandContext& ctx, Corner what, EntityId id) {
    const Entity* e = ctx.doc().find(id);
    if (!e)
        return;
    modify::ChamferOptions co;
    co.distance1 = getSetting(ctx.host(), kD1Key, 1.0);
    co.distance2 = getSetting(ctx.host(), kD2Key, 1.0);
    co.angle = getSetting(ctx.host(), kAngleKey, kPi / 4);
    co.byAngle = getFlag(ctx.host(), kByAngleKey, false);
    auto res = what == Corner::Fillet
                   ? modify::filletPolyline(*e, getSetting(ctx.host(), kRadiusKey, 0.0))
                   : modify::chamferPolyline(*e, co);
    if (!res) {
        ctx.message(res.error().message);
        return;
    }
    if (auto r = replaceEntity(ctx.tx(), id, res.value()); !r)
        ctx.message(r.error().message);
}

cmd::Task filletRun(cmd::CommandContext& ctx) {
    ctx.commitOnCancel();
    bool multiple = false;
    for (;;) {
        const double radius = getSetting(ctx.host(), kRadiusKey, 0.0);
        cmd::EntityOptions eo;
        eo.keywords = {"Polyline", "Radius", "Trim", "Multiple"};
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return filletable(doc, e); };
        auto first =
            co_await ctx.getEntity("Select first object or [Polyline/Radius/Trim/Multiple]", eo);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "Radius") {
                cmd::DistanceOptions d;
                d.defaultValue = radius;
                auto r = co_await ctx.getDistance("Specify fillet radius", d);
                if (r.ok())
                    putSetting(ctx.host(), kRadiusKey, r.value);
            } else if (first.keyword == "Trim") {
                cmd::KeywordOptions k;
                k.keywords = {"Trim", "No trim"};
                k.defaultKeyword = getFlag(ctx.host(), kFilletTrimKey, true) ? "Trim" : "No trim";
                auto r = co_await ctx.getKeyword("Trim mode [Trim/No trim]", k);
                if (r.ok())
                    putFlag(ctx.host(), kFilletTrimKey, r.keyword == "Trim");
            } else if (first.keyword == "Multiple") {
                multiple = true;
            } else if (first.keyword == "Polyline") {
                cmd::EntityOptions po;
                po.filter = [&doc = ctx.doc()](const Entity& e) {
                    return isEditable(doc, e) && kind(e) == EntityKind::Polyline;
                };
                auto p = co_await ctx.getEntity("Select polyline", po);
                if (p.ok())
                    polylineOp(ctx, Corner::Fillet, p.id);
                if (!multiple)
                    co_return;
            }
            continue;
        }
        if (!first.ok())
            co_return;
        cmd::EntityOptions so;
        so.filter = eo.filter;
        auto second =
            co_await ctx.getEntity("Select second object or Shift-select to apply corner", so);
        if (second.status == cmd::Status::Cancel)
            co_return;
        if (second.ok())
            cornerOp(ctx, Corner::Fillet, first, second);
        if (!multiple)
            co_return;
    }
}

cmd::Task chamferRun(cmd::CommandContext& ctx) {
    ctx.commitOnCancel();
    bool multiple = false;
    for (;;) {
        cmd::EntityOptions eo;
        eo.keywords = {"Polyline", "Distance", "Angle", "Trim", "Method", "Multiple"};
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return chamferable(doc, e); };
        auto first = co_await ctx.getEntity(
            "Select first line or [Polyline/Distance/Angle/Trim/Method/Multiple]", eo);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "Distance") {
                cmd::DistanceOptions d1;
                d1.defaultValue = getSetting(ctx.host(), kD1Key, 1.0);
                auto a = co_await ctx.getDistance("Specify first chamfer distance", d1);
                if (a.ok()) {
                    putSetting(ctx.host(), kD1Key, a.value);
                    cmd::DistanceOptions d2;
                    d2.defaultValue = a.value;
                    auto b = co_await ctx.getDistance("Specify second chamfer distance", d2);
                    if (b.ok()) {
                        putSetting(ctx.host(), kD2Key, b.value);
                        putFlag(ctx.host(), kByAngleKey, false);
                    }
                }
            } else if (first.keyword == "Angle") {
                cmd::DistanceOptions d1;
                d1.defaultValue = getSetting(ctx.host(), kD1Key, 1.0);
                auto a = co_await ctx.getDistance("Specify chamfer length on the first line", d1);
                if (a.ok()) {
                    putSetting(ctx.host(), kD1Key, a.value);
                    cmd::AngleOptions ao;
                    ao.defaultValue = getSetting(ctx.host(), kAngleKey, kPi / 4);
                    auto b = co_await ctx.getAngle("Specify chamfer angle from the first line", ao);
                    if (b.ok()) {
                        putSetting(ctx.host(), kAngleKey, b.value);
                        putFlag(ctx.host(), kByAngleKey, true);
                    }
                }
            } else if (first.keyword == "Method") {
                cmd::KeywordOptions k;
                k.keywords = {"Distance", "Angle"};
                k.defaultKeyword = getFlag(ctx.host(), kByAngleKey, false) ? "Angle" : "Distance";
                auto r = co_await ctx.getKeyword("Method [Distance/Angle]", k);
                if (r.ok())
                    putFlag(ctx.host(), kByAngleKey, r.keyword == "Angle");
            } else if (first.keyword == "Trim") {
                cmd::KeywordOptions k;
                k.keywords = {"Trim", "No trim"};
                k.defaultKeyword = getFlag(ctx.host(), kChamferTrimKey, true) ? "Trim" : "No trim";
                auto r = co_await ctx.getKeyword("Trim mode [Trim/No trim]", k);
                if (r.ok())
                    putFlag(ctx.host(), kChamferTrimKey, r.keyword == "Trim");
            } else if (first.keyword == "Multiple") {
                multiple = true;
            } else if (first.keyword == "Polyline") {
                cmd::EntityOptions po;
                po.filter = [&doc = ctx.doc()](const Entity& e) {
                    return isEditable(doc, e) && kind(e) == EntityKind::Polyline;
                };
                auto p = co_await ctx.getEntity("Select polyline", po);
                if (p.ok())
                    polylineOp(ctx, Corner::Chamfer, p.id);
                if (!multiple)
                    co_return;
            }
            continue;
        }
        if (!first.ok())
            co_return;
        cmd::EntityOptions so;
        so.filter = eo.filter;
        auto second =
            co_await ctx.getEntity("Select second line or Shift-select to apply corner", so);
        if (second.status == cmd::Status::Cancel)
            co_return;
        if (second.ok())
            cornerOp(ctx, Corner::Chamfer, first, second);
        if (!multiple)
            co_return;
    }
}

class FilletCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FILLET"; }
    std::vector<std::string> defaultAliases() const override { return {"F"}; }
    std::string_view description() const override { return "Round the corner between two objects"; }
    cmd::Task run(cmd::CommandContext& ctx) override { co_await filletRun(ctx); }
};

class ChamferCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CHAMFER"; }
    std::vector<std::string> defaultAliases() const override { return {"CHA"}; }
    std::string_view description() const override { return "Bevel the corner between two lines"; }
    cmd::Task run(cmd::CommandContext& ctx) override { co_await chamferRun(ctx); }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFillet() {
    return std::make_unique<FilletCommand>();
}
std::unique_ptr<cmd::Command> makeChamfer() {
    return std::make_unique<ChamferCommand>();
}

}  // namespace tbcad::modifycmd
