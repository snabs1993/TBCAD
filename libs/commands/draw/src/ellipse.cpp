#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

// Ellipse with the given centre and half major axis whose other half axis is `minor` long. A
// longer "minor" axis becomes the major one, so the ratio stays <= 1.
std::optional<EllipseData> ellipseFromAxes(Vec2 center, Vec2 halfMajor, double minor) {
    const double a = length(halfMajor);
    if (a < tol::kLinear || minor < tol::kLinear)
        return std::nullopt;
    EllipseData e;
    e.center = center;
    if (minor > a) {
        e.majorAxis = perp(halfMajor / a) * minor;
        e.ratio = a / minor;
    } else {
        e.majorAxis = halfMajor;
        e.ratio = minor / a;
    }
    return e;
}

// Parameter of the ellipse point that lies in direction `angle` (radians from the +x axis) as seen
// from the centre.
double paramOfDirection(const EllipseData& e, double angle) {
    const double rel = angle - std::atan2(e.majorAxis.y, e.majorAxis.x);
    return normalizeAngle(std::atan2(std::sin(rel) / e.ratio, std::cos(rel)));
}

// ELLIPSE [EL]: axis endpoints + distance to the other axis (or Rotation); Center starts from the
// centre; Arc limits the ellipse between a start and an end angle (measured from the major axis
// as seen from the centre, like a picked point would be).
class EllipseCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ELLIPSE"; }
    std::vector<std::string> defaultAliases() const override { return {"EL"}; }
    std::string_view description() const override { return "Draw an ellipse or elliptical arc"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        bool arc = false;
        Vec2 center{}, halfMajor{};

        cmd::PointOptions o;
        o.keywords = {"Arc", "Center"};
        // With isometric drafting on, circles can be drawn in the current isometric plane.
        const bool iso = ctx.host().setting("drafting.isodraft").value_or("0") == "1";
        if (iso)
            o.keywords.push_back("Isocircle");
        o.allowNone = false;
        auto p1 =
            co_await ctx.getPoint(promptWith("Specify axis endpoint of ellipse", o.keywords), o);
        if (p1.status == cmd::Status::Keyword && p1.keyword == "Isocircle") {
            auto cp = co_await ctx.getPoint("Specify center of isocircle");
            if (!cp.ok())
                co_return;
            cmd::DistanceOptions ro;
            ro.base = cp.point;
            ro.allowZero = false;
            auto r = co_await ctx.getDistance("Specify radius of isocircle", std::move(ro));
            if (!r.ok())
                co_return;
            // Axes of the plane are drawn at true length, so the circle's ellipse has its major
            // semi-axis sqrt(3/2) r along the direction at right angles to the plane's normal.
            const std::string plane = ctx.host().setting("drafting.isoplane").value_or("top");
            const double normalDeg = plane == "Left" || plane == "left"     ? 30.0
                                     : plane == "Right" || plane == "right" ? 150.0
                                                                            : 90.0;
            const double a = (normalDeg + 90.0) * kPi / 180.0;
            EllipseData e;
            e.center = cp.point;
            e.majorAxis = Vec2{std::cos(a), std::sin(a)} * (r.value * std::sqrt(1.5));
            e.ratio = 1.0 / std::sqrt(3.0);
            addEntity(ctx, e);
            co_return;
        }
        if (p1.status == cmd::Status::Keyword && p1.keyword == "Arc") {
            arc = true;
            cmd::PointOptions o2;
            o2.keywords = {"Center"};
            o2.allowNone = false;
            p1 = co_await ctx.getPoint(
                promptWith("Specify axis endpoint of elliptical arc", o2.keywords), o2);
        }
        if (p1.status == cmd::Status::Keyword) {  // Center
            auto cp = co_await ctx.getPoint("Specify center of ellipse");
            if (!cp.ok())
                co_return;
            cmd::PointOptions ao;
            ao.base = cp.point;
            ao.allowNone = false;
            ao.preview = [c, ctr = cp.point](Vec2 cursor, DisplayList& out) {
                previewLine(*c, ctr, cursor, out);
            };
            auto ep = co_await ctx.getPoint("Specify endpoint of axis", std::move(ao));
            if (!ep.ok())
                co_return;
            center = cp.point;
            halfMajor = ep.point - cp.point;
        } else if (p1.ok()) {
            cmd::PointOptions ao;
            ao.base = p1.point;
            ao.allowNone = false;
            ao.preview = [c, a = p1.point](Vec2 cursor, DisplayList& out) {
                previewLine(*c, a, cursor, out);
            };
            auto p2 = co_await ctx.getPoint("Specify other endpoint of axis", std::move(ao));
            if (!p2.ok())
                co_return;
            center = lerp(p1.point, p2.point, 0.5);
            halfMajor = (p2.point - p1.point) / 2;
        } else {
            co_return;
        }
        if (tol::isZeroLength(halfMajor)) {
            ctx.message("The axis has zero length.");
            co_return;
        }

        cmd::DistanceOptions dopt;
        dopt.base = center;
        dopt.allowZero = false;
        dopt.keywords = {"Rotation"};
        dopt.preview = [c, center, halfMajor](Vec2 cursor, DisplayList& out) {
            if (auto e = ellipseFromAxes(center, halfMajor, distance(center, cursor)))
                previewEntity(*c, makeEntity(*e), out);
        };
        auto d = co_await ctx.getDistance(
            promptWith("Specify distance to other axis", dopt.keywords), std::move(dopt));
        std::optional<EllipseData> e;
        if (d.ok()) {
            e = ellipseFromAxes(center, halfMajor, d.value);
        } else if (d.status == cmd::Status::Keyword) {
            cmd::AngleOptions ro;
            auto rot = co_await ctx.getAngle("Specify rotation around major axis", std::move(ro));
            if (!rot.ok())
                co_return;
            const double a = normalizeAngle(rot.value);
            if (a >= kPi / 2 - 1e-6 && a <= 3 * kPi / 2 + 1e-6) {
                ctx.message("The rotation must be between 0 and 89.9 degrees.");
                co_return;
            }
            const double ratio = std::cos(a);
            e = ellipseFromAxes(center, halfMajor, length(halfMajor) * ratio);
        } else {
            co_return;
        }
        if (!e) {
            ctx.message("The ellipse is degenerate.");
            co_return;
        }

        if (arc) {
            cmd::AngleOptions so;
            so.base = center;
            auto sa = co_await ctx.getAngle("Specify start angle", std::move(so));
            if (!sa.ok())
                co_return;
            const double startParam = paramOfDirection(*e, sa.value);
            cmd::AngleOptions eo;
            eo.base = center;
            eo.preview = [c, base = *e, startParam](Vec2 cursor, DisplayList& out) {
                EllipseData t = base;
                t.startParam = startParam;
                t.endParam = paramOfDirection(
                    base, std::atan2(cursor.y - base.center.y, cursor.x - base.center.x));
                previewEntity(*c, makeEntity(t), out);
            };
            auto ea = co_await ctx.getAngle("Specify end angle", std::move(eo));
            if (!ea.ok())
                co_return;
            e->startParam = startParam;
            e->endParam = paramOfDirection(*e, ea.value);
            if (tol::nearlyEqualAngle(e->startParam, e->endParam)) {
                ctx.message("The start and end angles are equal.");
                co_return;
            }
        }
        addEntity(ctx, *e);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeEllipse() {
    return std::make_unique<EllipseCommand>();
}

}  // namespace tbcad::draw::detail
