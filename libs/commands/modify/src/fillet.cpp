#include <cstdio>
#include <string>

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
    if (!isTrimTarget(doc, e))
        return false;
    switch (kind(e)) {
        case EntityKind::Ray:
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
    if (!isTrimTarget(doc, e))
        return false;
    switch (kind(e)) {
        case EntityKind::Ray:
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
        const std::size_t sa = modify::nearestSegmentIndex(*pl, first.pickPoint);
        const std::size_t sb = modify::nearestSegmentIndex(*pl, second.pickPoint);
        auto v = modify::sharedVertex(*pl, sa, sb);
        Entity reduced;
        if (!v) {
            // Two lines with one arc between them: the arc goes, the lines meet, the corner is
            // made.
            auto gone = modify::removeArcBetween(*a, sa, sb);
            if (!gone) {
                ctx.message("Those two segments do not share a corner.");
                return;
            }
            reduced = std::move(gone.value().polyline);
            v = gone.value().vertex;
            a = &reduced;
        }
        const bool sharp =
            fillet ? radius <= 0 : (co.distance1 <= 0 && co.distance2 <= 0 && !co.byAngle);
        auto res = sharp && &reduced == a ? Result<Entity>(reduced)
                   : fillet               ? modify::filletPolylineVertex(*a, *v, radius)
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

std::string fixed4(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

// Undo inside FILLET and CHAMFER: each corner is made after a checkpoint.
void undoCorner(cmd::CommandContext& ctx) {
    if (!ctx.rollbackToCheckpoint())
        ctx.message("Everything has been undone.");
}

cmd::Task filletRun(cmd::CommandContext& ctx) {
    ctx.commitOnCancel();
    bool multiple = false;
    // As AutoCAD reports it when the command starts.
    ctx.message(std::string("Current settings: Mode = ") +
                (getFlag(ctx.host(), kFilletTrimKey, true) ? "TRIM" : "NOTRIM") +
                ", Radius = " + fixed4(getSetting(ctx.host(), kRadiusKey, 0.0)));
    for (;;) {
        const double radius = getSetting(ctx.host(), kRadiusKey, 0.0);
        cmd::EntityOptions eo;
        eo.keywords = {"Undo", "Polyline", "Radius", "Trim", "Multiple"};
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return filletable(doc, e); };
        auto first = co_await ctx.getEntity(
            "Select first object or [Undo/Polyline/Radius/Trim/Multiple]", eo);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "Undo") {
                undoCorner(ctx);
            } else if (first.keyword == "Radius") {
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
                if (p.ok()) {
                    ctx.checkpoint();
                    polylineOp(ctx, Corner::Fillet, p.id);
                }
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
        if (second.ok()) {
            ctx.checkpoint();
            cornerOp(ctx, Corner::Fillet, first, second);
        }
        if (!multiple)
            co_return;
    }
}

cmd::Task chamferRun(cmd::CommandContext& ctx) {
    ctx.commitOnCancel();
    bool multiple = false;
    {
        const bool trim = getFlag(ctx.host(), kChamferTrimKey, true);
        std::string msg = std::string("(") + (trim ? "TRIM" : "NO TRIM") + " mode) ";
        if (getFlag(ctx.host(), kByAngleKey, false))
            msg += "Current chamfer Length = " + fixed4(getSetting(ctx.host(), kD1Key, 1.0)) +
                   ", Angle = " + fixed4(getSetting(ctx.host(), kAngleKey, kPi / 4) * 180 / kPi);
        else
            msg += "Current chamfer Dist1 = " + fixed4(getSetting(ctx.host(), kD1Key, 1.0)) +
                   ", Dist2 = " + fixed4(getSetting(ctx.host(), kD2Key, 1.0));
        ctx.message(msg);
    }
    for (;;) {
        cmd::EntityOptions eo;
        // mEthod as in AutoCAD: M picks Multiple, E picks the method.
        eo.keywords = {"Undo", "Polyline", "Distance", "Angle", "Trim", "mEthod", "Multiple"};
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return chamferable(doc, e); };
        auto first = co_await ctx.getEntity(
            "Select first line or [Undo/Polyline/Distance/Angle/Trim/mEthod/Multiple]", eo);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "Undo") {
                undoCorner(ctx);
            } else if (first.keyword == "Distance") {
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
            } else if (first.keyword == "mEthod") {
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
                if (p.ok()) {
                    ctx.checkpoint();
                    polylineOp(ctx, Corner::Chamfer, p.id);
                }
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
        if (second.ok()) {
            ctx.checkpoint();
            cornerOp(ctx, Corner::Chamfer, first, second);
        }
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
