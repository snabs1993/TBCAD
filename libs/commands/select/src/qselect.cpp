#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "common.h"
#include "tbcad/geom/geom.h"

namespace tbcad::select {

namespace detail {

std::string upper(std::string_view s) {
    return cmd::toUpperAscii(s);
}

std::optional<Color> parseColor(const std::string& text) {
    const std::string u = upper(text);
    if (u == "BYLAYER")
        return Color::byLayer();
    if (u == "BYBLOCK")
        return Color::byBlock();
    try {
        std::size_t used = 0;
        const int a = std::stoi(text, &used);
        if (used == text.size()) {
            if (a < 1 || a > 255)
                return std::nullopt;
            return Color::index(static_cast<std::uint8_t>(a));
        }
        const std::size_t c1 = text.find(',');
        const std::size_t c2 = c1 == std::string::npos ? c1 : text.find(',', c1 + 1);
        if (c2 == std::string::npos)
            return std::nullopt;
        const int g = std::stoi(text.substr(c1 + 1, c2 - c1 - 1));
        const int b = std::stoi(text.substr(c2 + 1));
        for (int v : {a, g, b})
            if (v < 0 || v > 255)
                return std::nullopt;
        return Color::rgb(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(g),
                          static_cast<std::uint8_t>(b));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<LineWeight> parseWeight(const std::string& text) {
    const std::string u = upper(text);
    if (u == "BYLAYER")
        return LineWeight::byLayer();
    if (u == "BYBLOCK")
        return LineWeight::byBlock();
    if (u == "DEFAULT")
        return LineWeight::defaultWeight();
    try {
        std::size_t used = 0;
        const double mm = std::stod(text, &used);
        if (used != text.size() || mm < 0 || mm > 2.11)
            return std::nullopt;
        return LineWeight::mm(mm);
    } catch (...) {
        return std::nullopt;
    }
}

bool sameColor(Color a, Color b) {
    return a == b;
}

}  // namespace detail

std::optional<std::vector<EntityId>> queryEntities(const Document& doc, const Query& q) {
    using namespace detail;
    std::optional<Color> color;
    std::optional<LineWeight> weight;
    if (q.property == Property::Color) {
        color = parseColor(q.value);
        if (!color)
            return std::nullopt;
    } else if (q.property == Property::Lineweight) {
        weight = parseWeight(q.value);
        if (!weight)
            return std::nullopt;
    }
    std::optional<double> number;
    const bool numeric = q.property == Property::Transparency || q.property == Property::Height ||
                         q.property == Property::Radius || q.property == Property::Length;
    if (numeric) {
        try {
            std::size_t used = 0;
            const double v = std::stod(q.value, &used);
            if (used != q.value.size())
                return std::nullopt;
            number = v;
        } catch (...) {
            return std::nullopt;
        }
    }
    if (!numeric && q.compare != Compare::Equals)
        return std::nullopt;
    const std::string want = upper(q.value);
    if (q.property == Property::Type) {
        static const char* const kKnown[] = {
            "LINE", "CIRCLE", "ARC",   "POLYLINE", "LWPOLYLINE", "POINT",     "ELLIPSE", "SPLINE",
            "TEXT", "MTEXT",  "HATCH", "INSERT",   "BLOCK",      "DIMENSION", "LEADER"};
        if (std::none_of(std::begin(kKnown), std::end(kKnown),
                         [&](const char* k) { return want == k; }))
            return std::nullopt;
    }

    std::vector<EntityId> out;
    // Level queries also see the dimmed objects of other levels (they are shown, just not
    // pickable); every other property only looks at the current level.
    for (const Entity* e :
         q.property == Property::Level ? shownOnAnyLevel(doc) : shownEntities(doc)) {
        bool hit = false;
        switch (q.property) {
            case Property::Level: {
                const Level* l = doc.level(e->common.level);
                hit = l && upper(l->name) == want;
                break;
            }
            case Property::Type: {
                const std::string k = kindName(*e);
                hit = want == k || (k == "POLYLINE" && want == "LWPOLYLINE") ||
                      (k == "INSERT" && want == "BLOCK");
                break;
            }
            case Property::Layer: {
                const Layer* l = doc.layer(e->common.layer);
                hit = l && upper(l->name) == want;
                break;
            }
            case Property::Color:
                hit = (color->kind == ColorKind::ByLayer || color->kind == ColorKind::ByBlock)
                          ? e->common.color == *color
                          : sameColor(resolvedColor(doc, *e), *color);
                break;
            case Property::Linetype: {
                if (want == "BYLAYER") {
                    hit = e->common.linetype == kLinetypeByLayer;
                } else if (want == "BYBLOCK") {
                    hit = e->common.linetype == kLinetypeByBlock;
                } else {
                    const Linetype* lt = doc.linetype(resolvedLinetype(doc, *e));
                    hit = lt && upper(lt->name) == want;
                }
                break;
            }
            case Property::Transparency:
            case Property::Height:
            case Property::Radius:
            case Property::Length: {
                std::optional<double> v;
                if (q.property == Property::Transparency) {
                    const std::int8_t t = e->common.transparency;
                    v = t >= 0 ? t : 0;
                } else if (q.property == Property::Height) {
                    if (const auto* t = entityAs<TextData>(*e))
                        v = t->height;
                    else if (const auto* m = entityAs<MTextData>(*e))
                        v = m->height;
                } else if (q.property == Property::Radius) {
                    if (const auto* c = entityAs<CircleData>(*e))
                        v = c->radius;
                    else if (const auto* a = entityAs<ArcData>(*e))
                        v = a->radius;
                } else {
                    const double len = geom::length(*e);
                    if (len > 0)
                        v = len;
                }
                if (v) {
                    const double d = *v - *number;
                    const double tol = 1e-9 * std::max(1.0, std::abs(*number));
                    hit = q.compare == Compare::Greater ? d > tol
                          : q.compare == Compare::Less  ? d < -tol
                                                        : std::abs(d) <= tol;
                }
                break;
            }
            case Property::Pattern: {
                if (const auto* h = entityAs<HatchData>(*e))
                    hit = want == (h->solid ? "SOLID" : upper(h->pattern));
                break;
            }
            case Property::Lineweight:
                hit = (weight->isByLayer() || weight->isByBlock())
                          ? e->common.lineweight == *weight
                          : resolvedLineweight(doc, *e) == *weight;
                break;
        }
        if (hit != q.negate)
            out.push_back(e->common.id);
    }
    return out;
}

namespace {

// What "same style" means per kind; objects without a style compare equal to each other.
std::string styleKey(const Entity& e) {
    if (const auto* t = entityAs<TextData>(e))
        return "T" + std::to_string(t->style.value);
    if (const auto* m = entityAs<MTextData>(e))
        return "T" + std::to_string(m->style.value);
    if (const auto* d = entityAs<DimensionData>(e))
        return "D" + std::to_string(d->style.value);
    if (const auto* h = entityAs<HatchData>(e))
        return h->solid ? "SOLID" : "H" + h->pattern;
    return {};
}

// Text height or radius, rounded so equal sizes compare equal; other kinds have no size.
long long sizeKey(const Entity& e) {
    double v = -1;
    if (const auto* t = entityAs<TextData>(e))
        v = t->height;
    else if (const auto* m = entityAs<MTextData>(e))
        v = m->height;
    else if (const auto* c = entityAs<CircleData>(e))
        v = c->radius;
    else if (const auto* a = entityAs<ArcData>(e))
        v = a->radius;
    return static_cast<long long>(std::llround(v * 1e6));
}

}  // namespace

std::vector<EntityId> similarEntities(const Document& doc, const Entity& ref, unsigned match) {
    using namespace detail;
    auto has = [&](Match m) { return (match & static_cast<unsigned>(m)) != 0; };
    std::vector<EntityId> out;
    for (const Entity* e : shownEntities(doc)) {
        if (has(Match::Type) && kind(*e) != kind(ref))
            continue;
        if (has(Match::Layer) && e->common.layer != ref.common.layer)
            continue;
        if (has(Match::Color) && !(resolvedColor(doc, *e) == resolvedColor(doc, ref)))
            continue;
        if (has(Match::Linetype) && resolvedLinetype(doc, *e) != resolvedLinetype(doc, ref))
            continue;
        if (has(Match::Lineweight) &&
            !(resolvedLineweight(doc, *e) == resolvedLineweight(doc, ref)))
            continue;
        if (has(Match::Level) && e->common.level != ref.common.level)
            continue;
        if (has(Match::Transparency) && e->common.transparency != ref.common.transparency)
            continue;
        if (has(Match::Style) && styleKey(*e) != styleKey(ref))
            continue;
        if (has(Match::Size) && sizeKey(*e) != sizeKey(ref))
            continue;
        out.push_back(e->common.id);
    }
    return out;
}

namespace detail {

const char* const kPropNames[11] = {"Type",       "Layer",  "Color",        "Linetype",
                                    "Lineweight", "Level",  "Transparency", "Pattern",
                                    "Height",     "Radius", "Length"};

cmd::Task askOperator(cmd::CommandContext& ctx, Query& q, std::string_view lead, bool& ok) {
    ok = false;
    const bool numeric = q.property == Property::Transparency || q.property == Property::Height ||
                         q.property == Property::Radius || q.property == Property::Length;
    cmd::KeywordOptions oo;
    oo.keywords = {"Equals", "Not"};
    if (numeric) {
        oo.keywords.push_back("Greater");
        oo.keywords.push_back("Less");
    }
    oo.defaultKeyword = "Equals";
    const std::string prompt = std::string(lead) +
                               (numeric ? " [Equals/Not/Greater/Less]" : " [Equals/Not]") +
                               " <Equals>";
    auto o = co_await ctx.getKeyword(prompt, std::move(oo));
    if (!o.ok())
        co_return;
    q.negate = o.keyword == "Not";
    q.compare = o.keyword == "Greater" ? Compare::Greater
                : o.keyword == "Less"  ? Compare::Less
                                       : Compare::Equals;
    ok = true;
}

namespace {

// QSELECT [QSEL]: selects the objects whose property has (or does not have) a value.
class QSelect final : public cmd::Command {
public:
    std::string_view name() const override { return "QSELECT"; }
    std::vector<std::string> defaultAliases() const override { return {"QSEL"}; }
    std::string_view description() const override { return "Select objects by a property value"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions po;
        po.keywords = {"Type",         "Layer",   "Color",  "Linetype", "Lineweight", "Level",
                       "Transparency", "Pattern", "Height", "Radius",   "Length"};
        po.defaultKeyword = "Type";
        auto p = co_await ctx.getKeyword(
            "Property [Type/Layer/Color/Linetype/Lineweight/Level/Transparency/Pattern/Height/"
            "Radius/Length] <Type>",
            std::move(po));
        if (!p.ok())
            co_return;
        Query q;
        for (int i = 0; i < 11; ++i)
            if (p.keyword == kPropNames[i])
                q.property = static_cast<Property>(i);

        auto v = co_await ctx.getString("Value");
        if (!v.ok())
            co_return;
        q.value = v.text;

        bool opOk = false;
        co_await askOperator(ctx, q, "Select objects that are", opOk);
        if (!opOk)
            co_return;

        cmd::KeywordOptions ao;
        ao.keywords = {"Replace", "Append"};
        ao.defaultKeyword = "Replace";
        auto a = co_await ctx.getKeyword("Apply to the selection [Replace/Append] <Replace>",
                                         std::move(ao));
        if (!a.ok())
            co_return;

        auto found = queryEntities(ctx.doc(), q);
        if (!found) {
            ctx.message("\"" + q.value + "\" is not a valid " + p.keyword + " value.");
            co_return;
        }
        std::vector<EntityId> ids = *found;
        if (a.keyword == "Append") {
            std::vector<EntityId> merged = ctx.host().selection();
            std::unordered_set<EntityId> have(merged.begin(), merged.end());
            for (EntityId id : ids)
                if (have.insert(id).second)
                    merged.push_back(id);
            ids = std::move(merged);
        }
        ctx.host().setSelection(ids);
        ctx.message(std::to_string(found->size()) + " object(s) found.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeQSelect() {
    return std::make_unique<QSelect>();
}

}  // namespace detail
}  // namespace tbcad::select
