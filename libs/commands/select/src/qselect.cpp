#include <algorithm>
#include <cmath>

#include "common.h"

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
        out.push_back(e->common.id);
    }
    return out;
}

namespace detail {
namespace {

// QSELECT [QSEL]: selects the objects whose property has (or does not have) a value.
class QSelect final : public cmd::Command {
public:
    std::string_view name() const override { return "QSELECT"; }
    std::vector<std::string> defaultAliases() const override { return {"QSEL"}; }
    std::string_view description() const override { return "Select objects by a property value"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions po;
        po.keywords = {"Type", "Layer", "Color", "Linetype", "Lineweight", "Level"};
        po.defaultKeyword = "Type";
        auto p = co_await ctx.getKeyword(
            "Property [Type/Layer/Color/Linetype/Lineweight/Level] <Type>", std::move(po));
        if (!p.ok())
            co_return;
        Query q;
        if (p.keyword == "Layer")
            q.property = Property::Layer;
        else if (p.keyword == "Color")
            q.property = Property::Color;
        else if (p.keyword == "Linetype")
            q.property = Property::Linetype;
        else if (p.keyword == "Lineweight")
            q.property = Property::Lineweight;
        else if (p.keyword == "Level")
            q.property = Property::Level;

        auto v = co_await ctx.getString("Value");
        if (!v.ok())
            co_return;
        q.value = v.text;

        cmd::KeywordOptions oo;
        oo.keywords = {"Equals", "Not"};
        oo.defaultKeyword = "Equals";
        auto o =
            co_await ctx.getKeyword("Select objects that are [Equals/Not] <Equals>", std::move(oo));
        if (!o.ok())
            co_return;
        q.negate = o.keyword == "Not";

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
            for (EntityId id : ids)
                if (std::find(merged.begin(), merged.end(), id) == merged.end())
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
