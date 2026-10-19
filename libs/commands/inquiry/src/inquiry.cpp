#include "tbcad/inquiry/inquiry.h"

#include <cstdio>
#include <memory>

#include "extra.h"
#include "tbcad/geom/geom.h"

namespace tbcad::inquiry {

namespace {

constexpr const char* kLastResultKey = "inquiry.lastresult";
constexpr const char* kClipboardKey = "clipboard.text";

Precision precisionOf(cmd::CommandContext& ctx) {
    if (auto s = ctx.host().setting("inquiry.precision")) {
        try {
            return std::clamp(std::stoi(*s), 0, 12);
        } catch (...) {
        }
    }
    return 4;
}

// Prints a result and remembers it for "copy last result".
void report(cmd::CommandContext& ctx, const std::string& text) {
    ctx.message(text);
    ctx.host().setSetting(kLastResultKey, text);
}

std::string ang(double rad, Precision p) {
    return formatAngle(rad, p, AngleUnit::Degrees, true);
}

DisplayList outlineOf(const Document& doc, const Entity& e) {
    DisplayList out;
    geom::DisplayContext dc;
    geom::buildDisplay(doc, e, dc, out);
    return out;
}

DisplayList polygonPreview(const std::vector<Vec2>& pts, Vec2 cursor) {
    DisplayList out;
    DisplayPolyline pl;
    pl.pts = pts;
    pl.pts.push_back(cursor);
    pl.pts.push_back(pts.front());
    out.items.emplace_back(std::move(pl));
    return out;
}

bool isKeyword(const std::string& got, const char* name) {
    return cmd::toUpperAscii(got) == cmd::toUpperAscii(name);
}

// ---- DIST [DI] ----------------------------------------------------------------------------------
class Dist final : public cmd::Command {
public:
    std::string_view name() const override { return "DIST"; }
    std::vector<std::string> defaultAliases() const override { return {"DI"}; }
    std::string_view description() const override {
        return "Measure the distance between two points";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto first = co_await ctx.getPoint("Specify first point");
        if (!first.ok())
            co_return;
        cmd::PointOptions next;
        next.base = first.point;
        next.allowNone = false;
        next.preview = [from = first.point](Vec2 cursor, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = {from, cursor};
            out.items.emplace_back(std::move(pl));
        };
        auto second = co_await ctx.getPoint("Specify second point", std::move(next));
        if (!second.ok())
            co_return;

        const Precision p = precisionOf(ctx);
        const Vec2 d = second.point - first.point;
        const double angle = length(d) < tol::kLinear ? 0 : normalizeAngle(std::atan2(d.y, d.x));
        report(ctx, "Distance = " + formatLengthIn(ctx.doc(), length(d), p) +
                        ", Angle in XY plane = " + ang(angle, p) +
                        ", Delta X = " + formatLengthIn(ctx.doc(), d.x, p) +
                        ", Delta Y = " + formatLengthIn(ctx.doc(), d.y, p));
    }
};

// ---- ID ---------------------------------------------------------------------------------------
class IdPoint final : public cmd::Command {
public:
    std::string_view name() const override { return "ID"; }
    std::string_view description() const override { return "Show the coordinates of a point"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto pt = co_await ctx.getPoint("Specify point");
        if (!pt.ok())
            co_return;
        const Precision p = precisionOf(ctx);
        report(ctx, "X = " + formatLengthIn(ctx.doc(), pt.point.x, p) +
                        ", Y = " + formatLengthIn(ctx.doc(), pt.point.y, p));
    }
};

// ---- AREA [AA] ----------------------------------------------------------------------------------
class Area final : public cmd::Command {
public:
    std::string_view name() const override { return "AREA"; }
    std::vector<std::string> defaultAliases() const override { return {"AA"}; }
    std::string_view description() const override {
        return "Measure the area and perimeter of points or an object";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        enum class Mode { Single, Add, Subtract };
        Mode mode = Mode::Single;
        double total = 0;
        const Precision p = precisionOf(ctx);
        DisplayList shown;  // kept alive while the host displays it

        for (;;) {
            cmd::PointOptions first;
            first.keywords = {"Object", "Add", "Subtract"};
            std::string prompt = mode == Mode::Add
                                     ? "(ADD mode) Specify first corner point or [Object/Subtract]"
                                 : mode == Mode::Subtract
                                     ? "(SUBTRACT mode) Specify first corner point or [Object/Add]"
                                     : "Specify first corner point or [Object/Add/Subtract]";
            if (mode == Mode::Add)
                first.keywords = {"Object", "Subtract"};
            else if (mode == Mode::Subtract)
                first.keywords = {"Object", "Add"};
            auto start = co_await ctx.getPoint(prompt, std::move(first));
            if (start.status == cmd::Status::Cancel) {
                ctx.host().setPreview(nullptr);
                co_return;
            }
            if (start.status == cmd::Status::None) {
                if (mode != Mode::Single)
                    report(ctx, "Total area = " + formatAreaIn(ctx.doc(), total, p));
                ctx.host().setPreview(nullptr);
                co_return;
            }
            if (start.status == cmd::Status::Keyword) {
                if (isKeyword(start.keyword, "Add")) {
                    mode = Mode::Add;
                    continue;
                }
                if (isKeyword(start.keyword, "Subtract")) {
                    mode = Mode::Subtract;
                    continue;
                }
                // Object
                cmd::EntityOptions eo;
                eo.filter = [](const Entity& e) { return areaOfEntity(e).has_value(); };
                auto picked = co_await ctx.getEntity("Select objects", std::move(eo));
                if (!picked.ok())
                    continue;
                const Entity* e = ctx.doc().find(picked.id);
                auto r = e ? areaOfEntity(*e) : std::nullopt;
                if (!r) {
                    ctx.message("That object has no area.");
                    continue;
                }
                shown = outlineOf(ctx.doc(), *e);
                ctx.host().setPreview(&shown);
                report(ctx, "Area = " + formatAreaIn(ctx.doc(), r->area, p) +
                                ", Perimeter = " + formatLengthIn(ctx.doc(), r->perimeter, p));
                if (mode == Mode::Single) {
                    ctx.host().setPreview(nullptr);
                    co_return;
                }
                total += mode == Mode::Add ? r->area : -r->area;
                report(ctx, "Total area = " + formatAreaIn(ctx.doc(), total, p));
                continue;
            }

            // Points mode.
            std::vector<Vec2> pts{start.point};
            for (;;) {
                cmd::PointOptions next;
                next.base = pts.back();
                next.preview = [pts](Vec2 cursor, DisplayList& out) {
                    out = polygonPreview(pts, cursor);
                };
                auto n = co_await ctx.getPoint("Specify next corner point or press ENTER for total",
                                               std::move(next));
                if (n.status == cmd::Status::Cancel) {
                    ctx.host().setPreview(nullptr);
                    co_return;
                }
                if (n.status != cmd::Status::Ok)
                    break;
                pts.push_back(n.point);
            }
            if (pts.size() < 3) {
                ctx.message("At least three points are needed.");
                continue;
            }
            AreaResult r = areaOfPoints(pts);
            report(ctx, "Area = " + formatAreaIn(ctx.doc(), r.area, p) +
                            ", Perimeter = " + formatLengthIn(ctx.doc(), r.perimeter, p));
            if (mode == Mode::Single) {
                ctx.host().setPreview(nullptr);
                co_return;
            }
            total += mode == Mode::Add ? r.area : -r.area;
            report(ctx, "Total area = " + formatAreaIn(ctx.doc(), total, p));
        }
    }
};

// ---- ANGLE ------------------------------------------------------------------------------------
// Returns the first and last point of the straight piece the pick lies on (line, or the nearest
// straight segment of a polyline).
bool straightPiece(const Entity& e, Vec2 pick, Vec2& a, Vec2& b) {
    if (const auto* l = entityAs<LineData>(e)) {
        a = l->a;
        b = l->b;
        return true;
    }
    if (const auto* pl = entityAs<PolylineData>(e)) {
        double best = 1e300;
        bool found = false;
        for (const auto& s : geom::segments(*pl)) {
            if (!s.curve.isLine())
                continue;
            double d = s.curve.distanceTo(pick);
            if (d < best) {
                best = d;
                a = s.p0;
                b = s.p1;
                found = true;
            }
        }
        return found;
    }
    return false;
}

class Angle final : public cmd::Command {
public:
    std::string_view name() const override { return "ANGLE"; }
    std::string_view description() const override {
        return "Measure the angle between two lines or at a vertex";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Precision p = precisionOf(ctx);
        cmd::EntityOptions eo;
        eo.keywords = {"3points"};
        eo.filter = [](const Entity& e) {
            Vec2 a, b;
            return straightPiece(e, {}, a, b);
        };
        auto first = co_await ctx.getEntity("Select first line or [3points]", eo);
        if (first.status == cmd::Status::Keyword) {
            auto vertex = co_await ctx.getPoint("Specify angle vertex");
            if (!vertex.ok())
                co_return;
            cmd::PointOptions po;
            po.base = vertex.point;
            po.allowNone = false;
            auto p1 = co_await ctx.getPoint("Specify first angle endpoint", po);
            if (!p1.ok())
                co_return;
            auto p2 = co_await ctx.getPoint("Specify second angle endpoint", po);
            if (!p2.ok())
                co_return;
            report(ctx, "Angle = " + ang(angleAtVertex(vertex.point, p1.point, p2.point), p));
            co_return;
        }
        if (!first.ok())
            co_return;
        auto second = co_await ctx.getEntity("Select second line", eo);
        if (!second.ok())
            co_return;
        const Entity* ea = ctx.doc().find(first.id);
        const Entity* eb = ctx.doc().find(second.id);
        Vec2 a0, a1, b0, b1;
        if (!ea || !eb || !straightPiece(*ea, first.pickPoint, a0, a1) ||
            !straightPiece(*eb, second.pickPoint, b0, b1)) {
            ctx.message("Select straight lines.");
            co_return;
        }
        report(ctx,
               "Angle = " +
                   ang(angleBetweenLines(a0, a1, first.pickPoint, b0, b1, second.pickPoint), p));
    }
};

// ---- LIST [LI] ----------------------------------------------------------------------------------
class List final : public cmd::Command {
public:
    std::string_view name() const override { return "LIST"; }
    std::vector<std::string> defaultAliases() const override { return {"LI"}; }
    std::string_view description() const override { return "List the properties of objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        const Precision p = precisionOf(ctx);
        // The text window is for reading: a selection of hundreds of thousands lists its first
        // objects and says how many were left out, instead of formatting every one of them.
        constexpr std::size_t kMaxListed = 300;
        std::string all;
        std::size_t listed = 0;
        for (EntityId id : sel.ids) {
            if (listed == kMaxListed)
                break;
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            ++listed;
            for (auto& line : describeEntity(ctx.doc(), *e, p)) {
                ctx.message(line);
                all += line + "\n";
            }
        }
        if (sel.ids.size() > listed) {
            const std::string more = std::to_string(sel.ids.size() - listed) +
                                     " more object(s) not listed; select fewer to see them.";
            ctx.message(more);
            all += more + "\n";
        }
        ctx.host().setSetting(kLastResultKey, all);
    }
};

// ---- COPYRESULT -------------------------------------------------------------------------------
class CopyResult final : public cmd::Command {
public:
    std::string_view name() const override { return "COPYRESULT"; }
    bool isTransparent() const override { return true; }
    std::string_view description() const override {
        return "Copy the last measurement result to the clipboard";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto last = ctx.host().setting(kLastResultKey);
        if (!last || last->empty()) {
            ctx.message("There is no measurement result to copy yet.");
            co_return;
        }
        // The application watches this key and puts the text on the system clipboard.
        ctx.host().setSetting(kClipboardKey, *last);
        ctx.message("Copied: " + *last);
        co_return;
    }
};

// ---- DBLIST -------------------------------------------------------------------------------------
class DbList final : public cmd::Command {
public:
    std::string_view name() const override { return "DBLIST"; }
    std::string_view description() const override { return "List every object in the drawing"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Precision p = precisionOf(ctx);
        std::string all;
        std::size_t n = 0;
        for (EntityId id : ctx.doc().modelSpace()) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            ++n;
            for (auto& line : describeEntity(ctx.doc(), *e, p)) {
                ctx.message(line);
                all += line + "\n";
            }
        }
        ctx.message(std::to_string(n) + " objects listed.");
        ctx.host().setSetting(kLastResultKey, all);
        co_return;
    }
};

// ---- MEASUREGEOM [MEA] --------------------------------------------------------------------------
class MeasureGeom final : public cmd::Command {
public:
    std::string_view name() const override { return "MEASUREGEOM"; }
    std::vector<std::string> defaultAliases() const override { return {"MEA"}; }
    std::string_view description() const override {
        return "Measure a distance, radius, angle or area";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Distance", "Radius", "Angle", "Area", "Volume", "Quick"};
        ko.defaultKeyword = "Distance";
        auto k = co_await ctx.getKeyword(
            "Measure [Distance/Radius/Angle/Area/Volume/Quick] <Distance>", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "Quick") {
            co_await makeQuickMeasure()->run(ctx);
            co_return;
        }
        if (k.keyword == "Volume") {
            // The area of a closed shape times a height: an extruded 2D shape.
            const Precision p = precisionOf(ctx);
            double area = 0;
            cmd::PointOptions first;
            first.keywords = {"Object"};
            auto start = co_await ctx.getPoint("Specify first corner point or [Object]", first);
            if (start.status == cmd::Status::Keyword) {
                cmd::EntityOptions eo;
                eo.filter = [](const Entity& e) { return areaOfEntity(e).has_value(); };
                auto picked = co_await ctx.getEntity("Select an object", std::move(eo));
                if (!picked.ok())
                    co_return;
                const Entity* e = ctx.doc().find(picked.id);
                auto r = e ? areaOfEntity(*e) : std::nullopt;
                if (!r) {
                    ctx.message("That object has no area.");
                    co_return;
                }
                area = r->area;
            } else if (start.ok()) {
                std::vector<Vec2> pts{start.point};
                for (;;) {
                    cmd::PointOptions next;
                    next.base = pts.back();
                    auto n = co_await ctx.getPoint(
                        "Specify next corner point or press ENTER for total", std::move(next));
                    if (n.status == cmd::Status::Cancel)
                        co_return;
                    if (n.status != cmd::Status::Ok)
                        break;
                    pts.push_back(n.point);
                }
                if (pts.size() < 3) {
                    ctx.message("At least three points are needed.");
                    co_return;
                }
                area = areaOfPoints(pts).area;
            } else {
                co_return;
            }
            cmd::DistanceOptions ho;
            ho.allowZero = false;
            auto height = co_await ctx.getDistance("Specify the height", ho);
            if (!height.ok())
                co_return;
            const LengthUnit u = ctx.doc().header().units;
            const double kk = mmPerUnit(u);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", p,
                          area * std::abs(height.value) / (kk * kk * kk));
            report(ctx, "Volume = " + std::string(buf) + " " + std::string(unitSuffix(u)) + "^3");
            co_return;
        }
        if (k.keyword == "Radius") {
            cmd::EntityOptions eo;
            eo.filter = [](const Entity& e) {
                return entityAs<CircleData>(e) || entityAs<ArcData>(e);
            };
            auto pick = co_await ctx.getEntity("Select an arc or circle", eo);
            if (!pick.ok())
                co_return;
            const Entity* e = ctx.doc().find(pick.id);
            if (!e)
                co_return;
            const Precision p = precisionOf(ctx);
            const double r = entityAs<CircleData>(*e) ? entityAs<CircleData>(*e)->radius
                                                      : entityAs<ArcData>(*e)->radius;
            report(ctx, "Radius = " + formatLengthIn(ctx.doc(), r, p) +
                            ", Diameter = " + formatLengthIn(ctx.doc(), 2 * r, p));
            co_return;
        }
        if (k.keyword == "Angle")
            co_await Angle().run(ctx);
        else if (k.keyword == "Area")
            co_await Area().run(ctx);
        else
            co_await Dist().run(ctx);
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<Dist>());
    (void)registry.add(std::make_unique<IdPoint>());
    (void)registry.add(std::make_unique<Area>());
    (void)registry.add(std::make_unique<Angle>());
    (void)registry.add(std::make_unique<List>());
    (void)registry.add(std::make_unique<CopyResult>());
    (void)registry.add(makeQuickMeasure());
    (void)registry.add(makeAreaTable());
    (void)registry.add(makeAudit());
    (void)registry.add(makeRecover());
    (void)registry.add(makeMassProp());
    (void)registry.add(makeDwgProps());
    (void)registry.add(makeFindStray());
    (void)registry.add(makePickTrace());
    (void)registry.add(makeCal());
    (void)registry.add(std::make_unique<DbList>());
    (void)registry.add(std::make_unique<MeasureGeom>());
}

}  // namespace tbcad::inquiry
