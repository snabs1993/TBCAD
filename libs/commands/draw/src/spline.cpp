#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

enum class Method { Fit, Cv };

// Clamped (or, when closed, periodic) uniform cubic B-spline over the control vertices.
std::optional<SplineData> controlSpline(const std::vector<Vec2>& pts, bool closed) {
    const int n = static_cast<int>(pts.size());
    if (n < 2)
        return std::nullopt;
    SplineData d;
    d.degree = std::min(3, n - 1);
    d.control = pts;
    if (closed) {
        if (n < 3)
            return std::nullopt;
        d.degree = std::min(3, n - 1);
        for (int i = 0; i < d.degree; ++i)
            d.control.push_back(pts[static_cast<std::size_t>(i)]);
        const int count = static_cast<int>(d.control.size()) + d.degree + 1;
        for (int i = 0; i < count; ++i)
            d.knots.push_back(i);
        d.closed = true;
        d.periodic = true;
        return d;
    }
    for (int i = 0; i <= d.degree; ++i)
        d.knots.push_back(0);
    for (int i = 1; i <= n - d.degree - 1; ++i)
        d.knots.push_back(i);
    for (int i = 0; i <= d.degree; ++i)
        d.knots.push_back(n - d.degree);
    return d;
}

std::optional<SplineData> fitSpline(const std::vector<Vec2>& pts, bool closed,
                                    std::optional<Vec2> startTan, std::optional<Vec2> endTan) {
    if (pts.size() < 2)
        return std::nullopt;
    geom::InterpolateOptions o;
    o.degree = static_cast<int>(std::min<std::size_t>(3, pts.size() - 1));
    o.closed = closed;
    o.startTangent = startTan;
    o.endTangent = endTan;
    auto r = geom::interpolateSpline(pts, o);
    if (!r)
        return std::nullopt;
    return r.value();
}

std::optional<SplineData> build(Method m, const std::vector<Vec2>& pts, bool closed,
                                std::optional<Vec2> st = {}, std::optional<Vec2> et = {}) {
    return m == Method::Fit ? fitSpline(pts, closed, st, et) : controlSpline(pts, closed);
}

// SPLINE [SPL]: fit points through geom::interpolateSpline (default) or control vertices
// (Method). Enter finishes; for fit splines it then asks for optional end tangents (Enter =
// natural). Close and Undo are options of the next-point prompt.
class SplineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SPLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"SPL"}; }
    std::string_view description() const override { return "Draw a spline"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        Method method = Method::Fit;
        std::vector<Vec2> pts;
        bool closed = false;

        for (;;) {
            cmd::PointOptions o;
            if (!pts.empty())
                o.base = pts.back();
            if (pts.empty())
                o.keywords = {"Method"};
            else {
                if (pts.size() >= 3)
                    o.keywords.push_back("Close");
                o.keywords.push_back("Undo");
            }
            o.preview = [c, method, pts](Vec2 cursor, DisplayList& out) {
                std::vector<Vec2> p = pts;
                p.push_back(cursor);
                if (auto s = build(method, p, false))
                    previewEntity(*c, makeEntity(*s), out);
            };
            auto r = co_await ctx.getPoint(
                promptWith(pts.empty() ? "Specify first point" : "Specify next point", o.keywords),
                std::move(o));
            if (r.status == cmd::Status::None)
                break;
            if (r.status == cmd::Status::Cancel)
                co_return;
            if (r.ok()) {
                if (pts.empty() || !tol::isZeroLength(r.point - pts.back()))
                    pts.push_back(r.point);
                else
                    ctx.message("The point coincides with the previous one.");
                continue;
            }
            if (r.keyword == "Method") {
                cmd::KeywordOptions ko;
                ko.keywords = {"Fit", "CV"};
                ko.defaultKeyword = method == Method::Fit ? "Fit" : "CV";
                auto m =
                    co_await ctx.getKeyword("Enter spline creation method [Fit/CV]", std::move(ko));
                if (!m.ok())
                    co_return;
                method = m.keyword == "Fit" ? Method::Fit : Method::Cv;
            } else if (r.keyword == "Undo") {
                pts.pop_back();
            } else {  // Close
                closed = true;
                break;
            }
        }

        if (pts.size() < 2) {
            ctx.message("A spline needs at least two points.");
            co_return;
        }
        std::optional<Vec2> startTan, endTan;
        if (method == Method::Fit && !closed) {
            cmd::PointOptions so;
            so.base = pts.front();
            so.preview = [c, method, pts](Vec2 cursor, DisplayList& out) {
                if (auto s = build(method, pts, false, cursor - pts.front()))
                    previewEntity(*c, makeEntity(*s), out);
            };
            auto st = co_await ctx.getPoint("Specify start tangent <natural>", std::move(so));
            if (st.status == cmd::Status::Cancel)
                co_return;
            if (st.ok())
                startTan = st.point - pts.front();
            cmd::PointOptions eo;
            eo.base = pts.back();
            eo.preview = [c, method, pts, startTan](Vec2 cursor, DisplayList& out) {
                if (auto s = build(method, pts, false, startTan, cursor - pts.back()))
                    previewEntity(*c, makeEntity(*s), out);
            };
            auto et = co_await ctx.getPoint("Specify end tangent <natural>", std::move(eo));
            if (et.status == cmd::Status::Cancel)
                co_return;
            if (et.ok())
                endTan = et.point - pts.back();
        }
        auto s = build(method, pts, closed, startTan, endTan);
        if (!s) {
            ctx.message("The spline could not be created from those points.");
            co_return;
        }
        addEntity(ctx, *s);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSpline() {
    return std::make_unique<SplineCommand>();
}

}  // namespace tbcad::draw::detail
