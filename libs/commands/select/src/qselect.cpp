#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <unordered_set>

#include "common.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"

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

// * any run of characters, ? one character; both already upper case.
bool wildcardMatch(std::string_view pat, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

// Name of the text or dimension style an object uses; empty for objects without one.
std::string styleName(const Document& doc, const Entity& e) {
    auto textStyle = [&](TextStyleId id) {
        const TextStyle* s = doc.textStyle(id ? id : doc.header().currentTextStyle);
        return s ? s->name : std::string();
    };
    auto dimStyle = [&](DimStyleId id) {
        const DimStyle* s = doc.dimStyle(id ? id : doc.header().currentDimStyle);
        return s ? s->name : std::string();
    };
    if (const auto* t = entityAs<TextData>(e))
        return textStyle(t->style);
    if (const auto* m = entityAs<MTextData>(e))
        return textStyle(m->style);
    if (const auto* d = entityAs<DimensionData>(e))
        return dimStyle(d->style);
    if (const auto* l = entityAs<LeaderData>(e))
        return dimStyle(l->style);
    if (const auto* ml = entityAs<MLeaderData>(e))
        return dimStyle(ml->style);
    return {};
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
    const bool wild = q.compare == Compare::Wildcard;
    if (wild && (numeric || q.property == Property::Color || q.property == Property::Lineweight))
        return std::nullopt;
    if (!numeric && q.compare != Compare::Equals && !wild)
        return std::nullopt;
    const std::string want = upper(q.value);
    // Text properties: equal ignoring case, or a * and ? pattern.
    auto same = [&](std::string_view have) {
        return wild ? wildcardMatch(want, upper(have)) : upper(have) == want;
    };
    if (q.property == Property::Type && !wild) {
        static const char* const kKnown[] = {
            "LINE",  "CIRCLE", "ARC",   "POLYLINE", "LWPOLYLINE",  "POINT",     "ELLIPSE", "SPLINE",
            "TEXT",  "MTEXT",  "HATCH", "INSERT",   "BLOCK",       "DIMENSION", "LEADER",  "PROXY",
            "TABLE", "RAY",    "XLINE", "MLINE",    "MULTILEADER", "MLEADER"};
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
                hit = l && same(l->name);
                break;
            }
            case Property::Type: {
                const std::string k = kindName(*e);
                hit = same(k) || (k == "POLYLINE" && want == "LWPOLYLINE") ||
                      (k == "INSERT" && want == "BLOCK") ||
                      (k == "MULTILEADER" && want == "MLEADER");
                break;
            }
            case Property::Layer: {
                const Layer* l = doc.layer(e->common.layer);
                hit = l && same(l->name);
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
                    hit = lt && same(lt->name);
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
                    hit = same(h->solid ? std::string("SOLID") : h->pattern);
                break;
            }
            case Property::Name: {
                if (const auto* i = entityAs<InsertData>(*e))
                    if (const BlockDef* b = doc.block(i->block))
                        hit = same(b->name);
                break;
            }
            case Property::Style: {
                const std::string st = styleName(doc, *e);
                hit = !st.empty() && same(st);
                break;
            }
            case Property::Contents: {
                if (const auto* t = entityAs<TextData>(*e))
                    hit = same(t->utf8);
                else if (const auto* m = entityAs<MTextData>(*e))
                    hit = same(text::plainText(m->utf8));
                else if (const auto* ml = entityAs<MLeaderData>(*e))
                    hit = ml->content == MLeaderData::Content::Text &&
                          same(text::plainText(ml->utf8));
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
    // A block reference's "style" is its block (AutoCAD's Name setting).
    if (const auto* i = entityAs<InsertData>(e))
        return "B" + std::to_string(i->block.value);
    if (const auto* m = entityAs<MLeaderData>(e))
        return "M" + std::to_string(m->style.value);
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

namespace {

// The values of the matched properties, as one string: equal strings mean "similar".
std::string similarKey(const Document& doc, const Entity& e, unsigned match) {
    auto has = [&](Match m) { return (match & static_cast<unsigned>(m)) != 0; };
    std::string k;
    auto put = [&](const auto& v) {
        k += std::to_string(v);
        k += '|';
    };
    if (has(Match::Type))
        put(static_cast<int>(kind(e)));
    if (has(Match::Layer))
        put(e.common.layer.value);
    if (has(Match::Color)) {
        const Color c = detail::resolvedColor(doc, e);
        put(static_cast<int>(c.kind));
        put(c.r);
        put(c.g);
        put(c.b);
    }
    if (has(Match::Linetype))
        put(detail::resolvedLinetype(doc, e).value);
    if (has(Match::Lineweight))
        put(detail::resolvedLineweight(doc, e).v);
    if (has(Match::Level))
        put(e.common.level.value);
    if (has(Match::Transparency))
        put(static_cast<int>(e.common.transparency));
    if (has(Match::Style)) {
        k += styleKey(e);
        k += '|';
    }
    if (has(Match::Size))
        put(sizeKey(e));
    return k;
}

}  // namespace

std::vector<EntityId> similarEntities(const Document& doc, const Entity& ref, unsigned match) {
    const EntityId id = ref.common.id;
    return similarEntities(doc, std::span<const EntityId>(&id, 1), match);
}

std::vector<EntityId> similarEntities(const Document& doc, std::span<const EntityId> refs,
                                      unsigned match) {
    using namespace detail;
    std::unordered_set<std::string> wanted;
    for (EntityId id : refs)
        if (const Entity* e = doc.find(id))
            wanted.insert(similarKey(doc, *e, match));
    std::vector<EntityId> out;
    if (wanted.empty())
        return out;
    for (const Entity* e : shownEntities(doc))
        if (wanted.count(similarKey(doc, *e, match)))
            out.push_back(e->common.id);
    return out;
}

namespace detail {

const char* const kPropNames[kPropCount] = {
    "Type",    "Layer",  "Color",  "Linetype", "Lineweight", "Level", "Transparency",
    "Pattern", "Height", "Radius", "Length",   "Name",       "Style", "Contents"};

std::string propertyPrompt() {
    std::string s = "Property [";
    for (int i = 0; i < kPropCount; ++i)
        s += std::string(i ? "/" : "") + kPropNames[i];
    return s + "] <Type>";
}

cmd::Task askOperator(cmd::CommandContext& ctx, Query& q, std::string_view lead, bool& ok) {
    ok = false;
    const bool numeric = q.property == Property::Transparency || q.property == Property::Height ||
                         q.property == Property::Radius || q.property == Property::Length;
    const bool text =
        !numeric && q.property != Property::Color && q.property != Property::Lineweight;
    cmd::KeywordOptions oo;
    oo.keywords = {"Equals", "Not"};
    if (numeric) {
        oo.keywords.push_back("Greater");
        oo.keywords.push_back("Less");
    }
    if (text)
        oo.keywords.push_back("Wildcard");
    oo.defaultKeyword = "Equals";
    const std::string prompt = std::string(lead) +
                               (numeric ? " [Equals/Not/Greater/Less]"
                                : text  ? " [Equals/Not/Wildcard]"
                                        : " [Equals/Not]") +
                               " <Equals>";
    auto o = co_await ctx.getKeyword(prompt, std::move(oo));
    if (!o.ok())
        co_return;
    q.negate = o.keyword == "Not";
    q.compare = o.keyword == "Greater"    ? Compare::Greater
                : o.keyword == "Less"     ? Compare::Less
                : o.keyword == "Wildcard" ? Compare::Wildcard
                                          : Compare::Equals;
    ok = true;
}

namespace {

// -QSELECT: selects the objects whose property has (or does not have) a value, typed.
class QSelect final : public cmd::Command {
public:
    std::string_view name() const override { return "-QSELECT"; }
    std::string_view description() const override {
        return "Select objects by a property value (command line)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions po;
        po.keywords.assign(kPropNames, kPropNames + kPropCount);
        po.defaultKeyword = "Type";
        auto p = co_await ctx.getKeyword(propertyPrompt(), std::move(po));
        if (!p.ok())
            co_return;
        Query q;
        for (int i = 0; i < kPropCount; ++i)
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
        // Within: only the objects selected now are searched (AutoCAD's "Apply to: Current
        // selection").
        ao.keywords = {"Replace", "Append", "Within"};
        ao.defaultKeyword = "Replace";
        auto a = co_await ctx.getKeyword("Apply to the selection [Replace/Append/Within] <Replace>",
                                         std::move(ao));
        if (!a.ok())
            co_return;

        auto found = queryEntities(ctx.doc(), q);
        if (!found) {
            ctx.message("\"" + q.value + "\" is not a valid " + p.keyword + " value.");
            co_return;
        }
        std::vector<EntityId> ids = *found;
        if (a.keyword == "Within") {
            const std::vector<EntityId>& now = ctx.host().selection();
            const std::unordered_set<EntityId> within(now.begin(), now.end());
            std::erase_if(ids, [&](EntityId id) { return !within.count(id); });
            ctx.host().setSelection(ids);
            ctx.message(std::to_string(ids.size()) + " object(s) found.");
            co_return;
        }
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

// QSELECT [QSEL]: the Quick Select dialog.
class QSelectDialog final : public cmd::Command {
public:
    std::string_view name() const override { return "QSELECT"; }
    std::vector<std::string> defaultAliases() const override { return {"QSEL"}; }
    std::string_view description() const override {
        return "Quick Select: select objects by type and a property value";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("qselect");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeQSelect() {
    return std::make_unique<QSelect>();
}

std::unique_ptr<cmd::Command> makeQSelectDialog() {
    return std::make_unique<QSelectDialog>();
}

}  // namespace detail
}  // namespace tbcad::select
