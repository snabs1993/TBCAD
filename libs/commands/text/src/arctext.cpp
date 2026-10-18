// ARCTEXT: text laid out along an arc or circle, one text object per letter.
#include <cmath>

#include "commands.h"
#include "tbcad/geom/entity.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kHalfPi = 1.5707963267948966;

std::vector<std::string> letters(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = 1;
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0)
            n = 4;
        else if (c >= 0xE0)
            n = 3;
        else if (c >= 0xC0)
            n = 2;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

class ArcTextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ARCTEXT"; }
    std::string_view description() const override {
        return "Put text along an arc or circle, one text object per letter";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) {
            return entityAs<ArcData>(e) != nullptr || entityAs<CircleData>(e) != nullptr;
        };
        auto pick = co_await ctx.getEntity("Select an arc or circle", eo);
        if (!pick.ok())
            co_return;
        const Entity* src = ctx.doc().find(pick.id);
        if (!src)
            co_return;
        Vec2 center{};
        double radius = 0, mid = kHalfPi, sweep = kTwoPi;
        if (const auto* a = entityAs<ArcData>(*src)) {
            center = a->center;
            radius = a->radius;
            sweep = std::fmod(a->endAngle - a->startAngle, kTwoPi);
            if (sweep <= 0)
                sweep += kTwoPi;
            mid = a->startAngle + sweep / 2;
        } else if (const auto* c = entityAs<CircleData>(*src)) {
            center = c->center;
            radius = c->radius;
        }
        if (!(radius > 0))
            co_return;

        auto txt = co_await ctx.getString("Enter text", cmd::StringOptions{});
        if (!txt.ok() || txt.text.empty())
            co_return;
        cmd::DistanceOptions ho;
        ho.defaultValue = 2.5;
        ho.allowZero = false;
        auto height = co_await ctx.getDistance("Text height <2.5>", std::move(ho));
        if (!height.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Outside", "Inside"};
        ko.defaultKeyword = "Outside";
        auto side = co_await ctx.getKeyword("Text side [Outside/Inside] <Outside>", ko);
        if (!side.ok())
            co_return;
        const bool outside = side.keyword != "Inside";
        if (!outside && !entityAs<ArcData>(*src))
            mid = -kHalfPi;  // a circle: inside text reads along the bottom

        const TextStyle* style = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        const double h = height.value;
        const auto chars = letters(txt.text);
        std::vector<double> w;
        double total = 0;
        for (const std::string& c : chars) {
            const double cw = c == " " ? 0.35 * h : text::measureWidth(style, c, h);
            w.push_back(cw);
            total += cw;
        }
        if (total > radius * sweep + 1e-9)
            ctx.message("The text is longer than the arc; it runs past the ends.");

        // Outside text reads clockwise (left to right along the top of a circle), inside text
        // counter-clockwise (left to right along the bottom).
        const double dir = outside ? -1 : 1;
        double pos = -total / 2;  // arc length from the middle to the start of the next letter
        ctx.setUndoLabel("ARCTEXT");
        for (std::size_t i = 0; i < chars.size(); ++i) {
            if (chars[i] != " ") {
                const double theta = mid + dir * (pos + w[i] / 2) / radius;
                const double rot = outside ? theta - kHalfPi : theta + kHalfPi;
                const Vec2 on = center + Vec2{std::cos(theta), std::sin(theta)} * radius;
                const Vec2 along{std::cos(rot), std::sin(rot)};
                TextData t;
                t.position = on - along * (w[i] / 2);
                t.height = h;
                t.rotation = rot;
                t.utf8 = chars[i];
                Entity e{{}, std::move(t)};
                applyCurrentProperties(ctx.doc().header(), e);
                if (auto r = ctx.tx().add(std::move(e)); !r) {
                    ctx.message(r.error().message);
                    co_return;
                }
            }
            pos += w[i];
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeArcTextCommand() {
    return std::make_unique<ArcTextCommand>();
}

}  // namespace tbcad::textcmd
