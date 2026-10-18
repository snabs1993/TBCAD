#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/symbolscmd/symbolscmd.h"
#include "tbcad/text/annotation_size.h"

namespace tbcad::symbolscmd {

namespace {

constexpr double kCell = 5.0;  // height of the frame and width of the symbol cell
constexpr double kText = 2.5;  // text height
constexpr double kPad = 1.2;   // between a divider and the text
constexpr double kPi_ = 3.14159265358979323846;

struct Sym {
    GdtSymbol symbol;
    const char* name;
};
constexpr Sym kSymbols[] = {
    {GdtSymbol::Straightness, "Straightness"},
    {GdtSymbol::Flatness, "Flatness"},
    {GdtSymbol::Circularity, "Circularity"},
    {GdtSymbol::Cylindricity, "Cylindricity"},
    {GdtSymbol::ProfileLine, "ProfileLine"},
    {GdtSymbol::ProfileSurface, "ProfileSurface"},
    {GdtSymbol::Angularity, "Angularity"},
    {GdtSymbol::Perpendicularity, "Perpendicularity"},
    {GdtSymbol::Parallelism, "Parallelism"},
    {GdtSymbol::Position, "Position"},
    {GdtSymbol::Concentricity, "Concentricity"},
    {GdtSymbol::Symmetry, "Symmetry"},
    {GdtSymbol::CircularRunout, "CircularRunout"},
    {GdtSymbol::TotalRunout, "TotalRunout"},
};

struct Builder {
    std::vector<Entity> out;
    Vec2 origin{};  // current cell centre offset

    void line(Vec2 a, Vec2 b) {
        Entity e;
        e.data = LineData{a, b};
        out.push_back(std::move(e));
    }
    void circle(Vec2 c, double r) {
        Entity e;
        e.data = CircleData{c, r};
        out.push_back(std::move(e));
    }
    void arc(Vec2 c, double r, double a0, double a1) {
        Entity e;
        e.data = ArcData{c, r, a0, a1};
        out.push_back(std::move(e));
    }
    void poly(std::vector<Vec2> pts, bool closed) {
        PolylineData p;
        p.closed = closed;
        for (Vec2 v : pts)
            p.v.push_back(PolyVertex{.p = v});
        Entity e;
        e.data = std::move(p);
        out.push_back(std::move(e));
    }
    void text(Vec2 at, double height, const std::string& s, HAlign h) {
        TextData t;
        t.position = t.alignPoint = at;
        t.height = height;
        t.utf8 = s;
        t.h = h;
        t.v = VAlign::Middle;
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        out.push_back(std::move(e));
    }
};

// The symbol, centred on (cx, cy), about 3 mm across.
void drawSymbol(Builder& b, GdtSymbol s, double cx, double cy) {
    auto P = [&](double x, double y) { return Vec2{cx + x, cy + y}; };
    auto arrow = [&](double x0, double y0, double x1, double y1) {
        b.line(P(x0, y0), P(x1, y1));
        const double a = std::atan2(y1 - y0, x1 - x0), L = 1.0, w = 0.4;
        const Vec2 tip = P(x1, y1);
        const Vec2 back = tip - Vec2{std::cos(a), std::sin(a)} * L;
        const Vec2 side{-std::sin(a) * w, std::cos(a) * w};
        b.poly({tip, back + side, back - side}, true);
    };
    switch (s) {
        case GdtSymbol::Straightness:
            b.line(P(-1.5, 0), P(1.5, 0));
            break;
        case GdtSymbol::Flatness:
            b.poly({P(-1.6, -0.7), P(0.8, -0.7), P(1.6, 0.7), P(-0.8, 0.7)}, true);
            break;
        case GdtSymbol::Circularity:
            b.circle(P(0, 0), 1.3);
            break;
        case GdtSymbol::Cylindricity:
            b.circle(P(0, 0), 1.0);
            b.line(P(-2.0, -1.1), P(-1.0, 1.1));
            b.line(P(1.0, -1.1), P(2.0, 1.1));
            break;
        case GdtSymbol::ProfileLine:
            b.arc(P(0, -0.8), 1.6, 0, kPi_);
            break;
        case GdtSymbol::ProfileSurface:
            b.arc(P(0, -0.8), 1.6, 0, kPi_);
            b.line(P(-1.6, -0.8), P(1.6, -0.8));
            break;
        case GdtSymbol::Angularity:
            b.line(P(-1.6, -1.2), P(1.6, -1.2));
            b.line(P(-1.6, -1.2), P(1.0, 1.4));
            break;
        case GdtSymbol::Perpendicularity:
            b.line(P(-1.6, -1.2), P(1.6, -1.2));
            b.line(P(0, -1.2), P(0, 1.4));
            break;
        case GdtSymbol::Parallelism:
            b.line(P(-1.5, -1.3), P(-0.2, 1.3));
            b.line(P(0.2, -1.3), P(1.5, 1.3));
            break;
        case GdtSymbol::Position:
            b.circle(P(0, 0), 1.4);
            b.line(P(-2.2, 0), P(2.2, 0));
            b.line(P(0, -2.2), P(0, 2.2));
            break;
        case GdtSymbol::Concentricity:
            b.circle(P(0, 0), 1.5);
            b.circle(P(0, 0), 0.8);
            break;
        case GdtSymbol::Symmetry:
            b.line(P(-1.6, 1.0), P(1.6, 1.0));
            b.line(P(-1.6, 0.0), P(1.6, 0.0));
            b.line(P(-1.6, -1.0), P(1.6, -1.0));
            break;
        case GdtSymbol::CircularRunout:
            arrow(-1.3, -1.3, 1.3, 1.3);
            break;
        case GdtSymbol::TotalRunout:
            arrow(-1.6, -1.2, 0.9, 1.3);
            arrow(-0.6, -1.5, 1.9, 1.0);
            break;
    }
}

// A material condition modifier: the letter in a small circle.
void drawModifier(Builder& b, char m, double x, double cy) {
    b.circle({x + 1.0, cy}, 1.0);
    b.text({x + 1.0, cy}, 1.4, std::string(1, m), HAlign::Center);
}

double textWidth(const std::string& s) {
    // About 0.62 of the text height per character; a glyph outside ASCII counts the same.
    double n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            n += 1;
    return n * kText * 0.62;
}

}  // namespace

const char* gdtName(GdtSymbol s) {
    for (const Sym& e : kSymbols)
        if (e.symbol == s)
            return e.name;
    return "";
}

std::vector<Entity> toleranceEntities(const ToleranceSpec& spec) {
    Builder b;
    double x = 0;  // left edge of the next cell
    const double top = kCell / 2, bottom = -kCell / 2;
    auto divider = [&](double at) { b.line({at, bottom}, {at, top}); };

    // The symbol cell.
    drawSymbol(b, spec.symbol, x + kCell / 2, 0);
    x += kCell;
    divider(x);

    // The tolerance: optional diameter sign, the value, the material modifier.
    std::string value = (spec.diameterZone ? "\xE2\x8C\x80" : "") + spec.value;  // U+2300
    double w = kPad + textWidth(value) + kPad;
    const double textAt = x + kPad;
    b.text({textAt, 0}, kText, value, HAlign::Left);
    if (spec.material) {
        drawModifier(b, spec.material, x + w - kPad + 0.2, 0);
        w += 2.2;
    }
    x += w;

    // The datums.
    for (const std::string& d : spec.datums) {
        if (d.empty())
            continue;
        divider(x);
        const double cw = std::max(kCell, 2 * kPad + textWidth(d));
        b.text({x + cw / 2, 0}, kText, d, HAlign::Center);
        x += cw;
    }

    // The outline.
    b.poly({{0, bottom}, {x, bottom}, {x, top}, {0, top}}, true);
    return std::move(b.out);
}

namespace {

class ToleranceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TOLERANCE"; }
    std::vector<std::string> defaultAliases() const override { return {"TOL"}; }
    std::string_view description() const override {
        return "Draw a geometric tolerance (feature control frame)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ToleranceSpec spec;
        cmd::KeywordOptions ko;
        for (const Sym& s : kSymbols)
            ko.keywords.emplace_back(s.name);
        ko.defaultKeyword = "Position";
        auto k =
            co_await ctx.getKeyword("Geometric characteristic [" + joined(ko.keywords) + "]", ko);
        if (!k.ok())
            co_return;
        for (const Sym& s : kSymbols)
            if (k.keyword == s.name)
                spec.symbol = s.symbol;

        cmd::StringOptions vo;
        auto v = co_await ctx.getString("Tolerance value, such as 0.05", vo);
        if (!v.ok())
            co_return;
        spec.value = cmd::trim(v.text);

        cmd::KeywordOptions dz;
        dz.keywords = {"Yes", "No"};
        dz.defaultKeyword = "No";
        auto z = co_await ctx.getKeyword("Diameter tolerance zone [Yes/No] <No>", dz);
        if (!z.ok())
            co_return;
        spec.diameterZone = z.keyword == "Yes";

        cmd::KeywordOptions mc;
        mc.keywords = {"None", "Mmc", "Lmc"};
        mc.defaultKeyword = "None";
        auto m = co_await ctx.getKeyword("Material condition [None/Mmc/Lmc] <None>", mc);
        if (!m.ok())
            co_return;
        spec.material = m.keyword == "Mmc" ? 'M' : m.keyword == "Lmc" ? 'L' : 0;

        static const char* kOrder[] = {"Primary", "Secondary", "Tertiary"};
        for (const char* which : kOrder) {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto d = co_await ctx.getString(std::string(which) + " datum letter <none>", so);
            if (!d.ok() || cmd::trim(d.text).empty())
                break;
            spec.datums.push_back(cmd::trim(d.text));
        }

        auto p = co_await ctx.getPoint("Specify the frame position (left edge, middle)");
        if (!p.ok())
            co_return;

        // One block per frame, inserted at the annotation scale like the symbols.
        std::string blockName;
        for (int i = 1;; ++i) {
            blockName = "TB_TOL_" + std::to_string(i);
            if (!ctx.doc().blockByName(blockName))
                break;
        }
        BlockDef def;
        def.name = blockName;
        auto id = ctx.tx().addBlock(std::move(def));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        for (Entity& e : toleranceEntities(spec)) {
            e.common.layer = kLayerZero;
            auto added = ctx.tx().add(std::move(e), id.value());
            if (!added) {
                ctx.message(added.error().message);
                co_return;
            }
        }
        const double n =
            text::suggestedAnnotationScale(ctx.doc(), ctx.host().view().scale,
                                           static_cast<bool>(ctx.host().drawingOwner()), kText);
        Entity e;
        e.data = Indirect<InsertData>(InsertData{id.value(), p.point, {n, n}, 0});
        applyCurrentProperties(ctx.doc().header(), e);
        e.common.annotative = true;
        e.common.annoScales = ScaleList(std::vector<double>{n});
        auto ins = ctx.tx().add(std::move(e));
        if (!ins) {
            ctx.message(ins.error().message);
            co_return;
        }
        ctx.setUndoLabel("TOLERANCE");
    }

private:
    static std::string joined(const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& k : v)
            s += (s.empty() ? "" : "/") + k;
        return s;
    }
};

}  // namespace

void registerToleranceCommand(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ToleranceCommand>());
}

}  // namespace tbcad::symbolscmd
