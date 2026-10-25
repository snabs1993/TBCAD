#include "tbcad/drafting/point_parser.h"

#include <cctype>
#include <cmath>

#include "tbcad/core/units.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"

namespace tbcad::drafting {

namespace {

std::optional<double> length(std::string_view s, LengthUnit unit) {
    std::string t = cmd::trim(s);
    if (t.empty())
        return std::nullopt;
    auto r = parseLength(t, unit);
    if (!r)
        return std::nullopt;
    return r.value();
}

std::optional<double> angle(std::string_view s, AngleUnit unit) {
    std::string t = cmd::trim(s);
    if (t.empty())
        return std::nullopt;
    auto r = parseAngle(t, unit);
    if (!r)
        return std::nullopt;
    return r.value();
}

}  // namespace

std::optional<Vec2> parseTypedPoint(std::string_view text, const cmd::PointParseContext& ctx,
                                    std::optional<Vec2> direction, const Ucs& ucs) {
    std::string t = cmd::trim(text);
    if (t.empty())
        return std::nullopt;
    bool relative = false;
    bool forced = false;
    if (t[0] == '@') {
        relative = true;
        t.erase(0, 1);
        if (cmd::trim(t).empty())
            return ctx.lastPoint;
    } else if (t[0] == '#') {
        forced = true;
        t.erase(0, 1);
    }
    t = cmd::trim(t);
    if (t.empty())
        return std::nullopt;
    // Typed numbers are in the current UCS: absolute points start at its origin, relative ones
    // at the last point, and both are turned by its rotation.
    const Vec2 origin = relative ? ctx.lastPoint : ucs.origin;

    if (auto lt = t.find('<'); lt != std::string::npos) {
        auto d = length(std::string_view(t).substr(0, lt), ctx.unit);
        auto a = angle(std::string_view(t).substr(lt + 1), ctx.angleUnit);
        if (!d || !a)
            return std::nullopt;
        return origin + ucs.dirToWorld(Vec2{std::cos(*a), std::sin(*a)} * *d);
    }
    if (auto comma = t.find(','); comma != std::string::npos) {
        if (t.find(',', comma + 1) != std::string::npos)
            return std::nullopt;  // x,y,z is not a 2D point
        auto x = length(std::string_view(t).substr(0, comma), ctx.unit);
        auto y = length(std::string_view(t).substr(comma + 1), ctx.unit);
        if (!x || !y)
            return std::nullopt;
        return origin + ucs.dirToWorld(Vec2{*x, *y});
    }
    // A single number is a distance from the base point, only when typed without @ or #.
    if (relative || forced || !ctx.base)
        return std::nullopt;
    auto d = length(t, ctx.unit);
    if (!d)
        return std::nullopt;
    Vec2 dir{};
    if (direction && lengthSquared(*direction) > 0)
        dir = normalized(*direction);
    else
        dir = normalized(ctx.cursor - *ctx.base);
    if (lengthSquared(dir) == 0)
        return std::nullopt;  // the cursor sits on the base point: no direction to measure along
    return *ctx.base + dir * *d;
}

double TypedPointParser::ucsAngle() const {
    return env_ ? currentUcs(env_->doc, env_->host).angle : 0;
}

std::optional<Vec2> TypedPointParser::parsePoint(std::string_view text,
                                                 const cmd::PointParseContext& ctx) {
    std::optional<Vec2> dir;
    if (env_ && env_->tracking && ctx.base && env_->tracking->base == *ctx.base &&
        env_->tracking->rawCursor == ctx.cursor)
        dir = env_->tracking->direction;
    return parseTypedPoint(text, ctx, dir, env_ ? currentUcs(env_->doc, env_->host) : Ucs{});
}

std::optional<std::string> TypedPointParser::snapOverride(std::string_view word) const {
    std::string w;
    for (char c : word)
        w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // AutoCAD's short forms; a full mode name works too. Other words (a keyword, a note typed at
    // a free-text prompt) are left alone.
    static const std::pair<const char*, const char*> kShort[] = {
        {"end", "endpoint"},      {"endp", "endpoint"},      {"mid", "midpoint"},
        {"int", "intersection"},  {"app", "apparent"},       {"appint", "apparent"},
        {"cen", "center"},        {"qua", "quadrant"},       {"quad", "quadrant"},
        {"per", "perpendicular"}, {"perp", "perpendicular"}, {"tan", "tangent"},
        {"nea", "nearest"},       {"near", "nearest"},       {"nod", "node"},
        {"ins", "insertion"},     {"ext", "extension"},      {"par", "parallel"},
        {"gce", "geometric"},     {"non", "none"},           {"none", "none"}};
    for (const auto& [shortForm, mode] : kShort)
        if (w == shortForm)
            return std::string(mode);
    for (int i = 0; i < kSnapModeCount; ++i)
        if (w == snapModeName(static_cast<SnapMode>(i)))
            return w;
    return std::nullopt;
}

void TypedPointParser::trackPoint(Vec2 p) {
    if (env_)
        env_->tempTrack.push_back(p);
}

}  // namespace tbcad::drafting
