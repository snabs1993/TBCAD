#include <algorithm>
#include <charconv>
#include <cmath>

#include "dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"

namespace tbcad::dimcmd {

namespace {

bool parseReal(std::string_view s, double& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && std::isfinite(out);
}

bool parseInt(std::string_view s, int& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

bool same(std::string_view a, std::string_view b) {
    return cmd::toUpperAscii(a) == cmd::toUpperAscii(b);
}

bool parseBool(std::string_view v, bool& out) {
    std::string u = cmd::toUpperAscii(v);
    if (u == "SUPPRESS" || u == "ON" || u == "YES" || u == "TRUE" || u == "1") {
        out = true;
        return true;
    }
    if (u == "SHOW" || u == "OFF" || u == "NO" || u == "FALSE" || u == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parseColor(std::string_view v, Color& out) {
    std::string u = cmd::toUpperAscii(v);
    if (u == "BYBLOCK") {
        out = Color::byBlock();
        return true;
    }
    if (u == "BYLAYER") {
        out = Color::byLayer();
        return true;
    }
    int idx = 0;
    if (parseInt(v, idx) && idx >= 0 && idx <= 255) {
        out = Color::index(static_cast<std::uint8_t>(idx));
        return true;
    }
    int rgb[3];
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        std::size_t comma = v.find(',', at);
        std::string_view part = v.substr(at, comma == std::string_view::npos ? comma : comma - at);
        if (!parseInt(part, rgb[i]) || rgb[i] < 0 || rgb[i] > 255)
            return false;
        if (comma == std::string_view::npos) {
            if (i != 2)
                return false;
            break;
        }
        at = comma + 1;
    }
    out = Color::rgb(static_cast<std::uint8_t>(rgb[0]), static_cast<std::uint8_t>(rgb[1]),
                     static_cast<std::uint8_t>(rgb[2]));
    return true;
}

std::string spaced(std::string_view v) {
    std::string s(v);
    if (same(s, "none"))
        return {};
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}

const std::vector<std::string> kProps = {
    "TextHeight",   "TextGap",       "TextStyle", "ArrowType", "ArrowSize",    "ExtOffset",
    "ExtExtension", "LineExtension", "Placement", "Alignment", "Units",        "Precision",
    "Prefix",       "Suffix",        "RoundOff",  "Scale",     "LeadingZeros", "TrailingZeros",
    "LineColor",    "ExtColor",      "TextColor"};

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string colourText(Color c) {
    switch (c.kind) {
        case ColorKind::ByBlock:
            return "ByBlock";
        case ColorKind::ByLayer:
            return "ByLayer";
        case ColorKind::Rgb:
            return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
        default:
            return std::to_string(c.indexValue());
    }
}

// Every property the DIMSTYLE command edits, as text, in the order of kProps.
std::vector<std::pair<std::string, std::string>> describe(const Document& doc, const DimStyle& s) {
    const TextStyle* ts = s.textStyle.value ? doc.textStyle(s.textStyle) : nullptr;
    const char* arrow[] = {"Filled", "Open", "Tick", "Dot", "None"};
    const char* unit[] = {"mm", "cm", "m", "in", "ft"};
    return {
        {"TextHeight", num(s.textHeight)},
        {"TextGap", num(s.textGap)},
        {"TextStyle", ts ? ts->name : "Current"},
        {"ArrowType", arrow[static_cast<int>(s.arrowType)]},
        {"ArrowSize", num(s.arrowSize)},
        {"ExtOffset", num(s.extLineOffset)},
        {"ExtExtension", num(s.extLineExtension)},
        {"LineExtension", num(s.dimLineExtension)},
        {"Placement", s.textPlacement == DimTextPlacement::AboveLine ? "Above" : "Centered"},
        {"Alignment", s.textAlign == DimTextAlign::WithDimLine ? "WithLine" : "Horizontal"},
        {"Units", unit[static_cast<int>(s.units)]},
        {"Precision", std::to_string(s.precision)},
        {"Prefix", s.prefix.empty() ? "none" : s.prefix},
        {"Suffix", s.suffix.empty() ? "none" : s.suffix},
        {"RoundOff", num(s.roundOff)},
        {"Scale", num(s.overallScale)},
        {"LeadingZeros", s.suppressLeadingZeros ? "Suppress" : "Show"},
        {"TrailingZeros", s.suppressTrailingZeros ? "Suppress" : "Show"},
        {"LineColor", colourText(s.dimLineColor)},
        {"ExtColor", colourText(s.extLineColor)},
        {"TextColor", colourText(s.textColor)},
    };
}

}  // namespace

const std::vector<std::string>& propertyKeywords() {
    return kProps;
}

std::string applyProperty(const Document& doc, DimStyle& s, std::string_view key,
                          std::string_view value) {
    auto real = [&](double& field, bool positive) -> std::string {
        double v = 0;
        if (!parseReal(value, v))
            return "Not a number: " + std::string(value);
        if (positive ? !(v > 0) : v < 0)
            return positive ? "The value must be greater than zero."
                            : "The value must not be negative.";
        field = v;
        return {};
    };
    auto colour = [&](Color& field) -> std::string {
        Color c;
        if (!parseColor(value, c))
            return "Colour is ByBlock, ByLayer, an index 0-255 or R,G,B.";
        field = c;
        return {};
    };
    if (same(key, "TextHeight"))
        return real(s.textHeight, true);
    if (same(key, "TextGap"))
        return real(s.textGap, false);
    if (same(key, "ArrowSize"))
        return real(s.arrowSize, true);
    if (same(key, "ExtOffset"))
        return real(s.extLineOffset, false);
    if (same(key, "ExtExtension"))
        return real(s.extLineExtension, false);
    if (same(key, "LineExtension"))
        return real(s.dimLineExtension, false);
    if (same(key, "RoundOff"))
        return real(s.roundOff, false);
    if (same(key, "Scale"))
        return real(s.overallScale, true);
    if (same(key, "Precision")) {
        int p = 0;
        if (!parseInt(value, p) || p < 0 || p > 8)
            return "Precision is a whole number from 0 to 8.";
        s.precision = p;
        return {};
    }
    if (same(key, "TextStyle")) {
        if (value.empty() || same(value, "Current")) {
            s.textStyle = {};
            return {};
        }
        const TextStyle* t = doc.textStyleByName(value);
        if (!t)
            return "No text style named \"" + std::string(value) + "\".";
        s.textStyle = t->id;
        return {};
    }
    if (same(key, "ArrowType")) {
        if (same(value, "Filled") || same(value, "Closed"))
            s.arrowType = ArrowType::ClosedFilled;
        else if (same(value, "Open"))
            s.arrowType = ArrowType::Open30;
        else if (same(value, "Tick") || same(value, "Architectural"))
            s.arrowType = ArrowType::ArchTick;
        else if (same(value, "Dot"))
            s.arrowType = ArrowType::Dot;
        else if (same(value, "None"))
            s.arrowType = ArrowType::None;
        else
            return "Arrow type is Filled, Open, Tick, Dot or None.";
        return {};
    }
    if (same(key, "Placement")) {
        if (same(value, "Above"))
            s.textPlacement = DimTextPlacement::AboveLine;
        else if (same(value, "Centered"))
            s.textPlacement = DimTextPlacement::Centered;
        else
            return "Placement is Above or Centered.";
        return {};
    }
    if (same(key, "Alignment")) {
        if (same(value, "WithLine") || same(value, "Aligned"))
            s.textAlign = DimTextAlign::WithDimLine;
        else if (same(value, "Horizontal"))
            s.textAlign = DimTextAlign::Horizontal;
        else
            return "Alignment is WithLine or Horizontal.";
        return {};
    }
    if (same(key, "Units")) {
        if (same(value, "mm"))
            s.units = LengthUnit::Millimeter;
        else if (same(value, "cm"))
            s.units = LengthUnit::Centimeter;
        else if (same(value, "m"))
            s.units = LengthUnit::Meter;
        else if (same(value, "in"))
            s.units = LengthUnit::Inch;
        else if (same(value, "ft"))
            s.units = LengthUnit::Foot;
        else
            return "Units are mm, cm, m, in or ft.";
        return {};
    }
    if (same(key, "Prefix")) {
        s.prefix = spaced(value);
        return {};
    }
    if (same(key, "Suffix")) {
        s.suffix = spaced(value);
        return {};
    }
    if (same(key, "LeadingZeros") || same(key, "TrailingZeros")) {
        bool b = false;
        if (!parseBool(value, b))
            return "Answer Suppress or Show.";
        (same(key, "LeadingZeros") ? s.suppressLeadingZeros : s.suppressTrailingZeros) = b;
        return {};
    }
    if (same(key, "LineColor"))
        return colour(s.dimLineColor);
    if (same(key, "ExtColor"))
        return colour(s.extLineColor);
    if (same(key, "TextColor"))
        return colour(s.textColor);
    return "Unknown property \"" + std::string(key) + "\".";
}

namespace {

class DimStyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMSTYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"D"}; }
    std::string_view description() const override {
        return "Create, select, rename, edit or delete dimension styles";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();
        ctx.setUndoLabel("Dimension style");
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Current", "New", "Rename", "Delete", "Edit", "Compare", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Dimension style option", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            if (k.keyword == "List") {
                list(ctx);
                continue;
            }
            cmd::StringOptions so;
            so.defaultValue = currentName(ctx);
            auto n = co_await ctx.getString(
                k.keyword == "New" ? "Name of the new style" : "Dimension style name", so);
            if (!n.ok())
                continue;
            std::string name = cmd::trim(n.text);
            if (k.keyword == "Current") {
                const DimStyle* s = ctx.doc().dimStyleByName(name);
                if (!s)
                    ctx.message("No dimension style named \"" + name + "\".");
                else
                    check(ctx, ctx.tx().modifyHeader(
                                   [id = s->id](Header& h) { h.currentDimStyle = id; }));
            } else if (k.keyword == "New") {
                if (name.empty() || ctx.doc().dimStyleByName(name)) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                DimStyle copy = dim::styleOf(ctx.doc(), ctx.doc().header().currentDimStyle);
                copy.id = {};
                copy.name = name;
                auto id = ctx.tx().addDimStyle(std::move(copy));
                if (!id)
                    ctx.message(id.error().message);
                else
                    check(ctx, ctx.tx().modifyHeader(
                                   [i = id.value()](Header& h) { h.currentDimStyle = i; }));
            } else if (k.keyword == "Rename") {
                const DimStyle* s = ctx.doc().dimStyleByName(name);
                if (!s) {
                    ctx.message("No dimension style named \"" + name + "\".");
                    continue;
                }
                if (s->id == kDimStyleStandard) {
                    ctx.message("The Standard style cannot be renamed.");
                    continue;
                }
                const DimStyleId id = s->id;
                cmd::StringOptions no;
                auto nn = co_await ctx.getString("New name", no);
                if (!nn.ok())
                    continue;
                const std::string to = cmd::trim(nn.text);
                if (to.empty() || ctx.doc().dimStyleByName(to)) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                check(ctx, ctx.tx().modifyDimStyle(id, [&](DimStyle& row) { row.name = to; }));
            } else if (k.keyword == "Delete") {
                const DimStyle* s = ctx.doc().dimStyleByName(name);
                if (!s)
                    ctx.message("No dimension style named \"" + name + "\".");
                else if (s->id == ctx.doc().header().currentDimStyle)
                    ctx.message("The current style cannot be deleted; make another one current.");
                else if (s->id == kDimStyleStandard)
                    ctx.message("The Standard style cannot be deleted.");
                else {
                    auto r = ctx.tx().eraseDimStyle(s->id);
                    if (!r)
                        ctx.message(r.error().code == ErrorCode::Conflict
                                        ? "Dimensions or leaders still use \"" + name + "\"."
                                        : r.error().message);
                }
            } else if (k.keyword == "Compare") {
                const DimStyle* a = ctx.doc().dimStyleByName(name);
                if (!a) {
                    ctx.message("No dimension style named \"" + name + "\".");
                    continue;
                }
                cmd::StringOptions so2;
                auto n2 = co_await ctx.getString("Style to compare with", so2);
                if (!n2.ok())
                    continue;
                const std::string other = cmd::trim(n2.text);
                const DimStyle* b = ctx.doc().dimStyleByName(other);
                if (!b) {
                    ctx.message("No dimension style named \"" + other + "\".");
                    continue;
                }
                const auto da = describe(ctx.doc(), *a), db = describe(ctx.doc(), *b);
                int differences = 0;
                for (std::size_t i = 0; i < da.size(); ++i)
                    if (da[i].second != db[i].second) {
                        ++differences;
                        ctx.message(da[i].first + ": " + da[i].second + " | " + db[i].second);
                    }
                ctx.message(differences == 0 ? "The styles are the same."
                                             : std::to_string(differences) + " difference(s).");
            } else if (k.keyword == "Edit") {
                const DimStyle* s = ctx.doc().dimStyleByName(name);
                if (!s) {
                    ctx.message("No dimension style named \"" + name + "\".");
                    continue;
                }
                const DimStyleId id = s->id;
                for (;;) {
                    cmd::KeywordOptions po;
                    po.keywords = propertyKeywords();
                    po.keywords.push_back("Done");
                    po.defaultKeyword = "Done";
                    auto p = co_await ctx.getKeyword("Property of \"" + name + "\"", po);
                    if (!p.ok() || p.keyword == "Done")
                        break;
                    cmd::StringOptions vo;
                    auto v = co_await ctx.getString(p.keyword + " value", vo);
                    if (!v.ok())
                        continue;
                    std::string err;
                    auto r = ctx.tx().modifyDimStyle(id, [&](DimStyle& row) {
                        err = applyProperty(ctx.doc(), row, p.keyword, cmd::trim(v.text));
                    });
                    if (!err.empty())
                        ctx.message(err);
                    else
                        check(ctx, r);
                }
            }
        }
    }

private:
    static std::string currentName(cmd::CommandContext& ctx) {
        return dim::styleOf(ctx.doc(), ctx.doc().header().currentDimStyle).name;
    }
    static void list(cmd::CommandContext& ctx) {
        for (const auto& [id, s] : ctx.doc().dimStyles())
            ctx.message((id == ctx.doc().header().currentDimStyle ? "* " : "  ") + s.name);
    }
    static void check(cmd::CommandContext& ctx, const Result<void>& r) {
        if (!r)
            ctx.message(r.error().message);
    }
};

// DIMSETUP: gives the selected dimensions and leaders a style, or an overall scale, in one step.
// A scale is a style of its own ("<style>-1to<N>", made from the style they use), so only the
// selected objects change.
class DimSetupCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMSETUP"; }
    std::string_view description() const override {
        return "Set the style or overall scale of the selected dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection(
            "Select dimensions or leaders", cmd::SelectionOptions{.filter = [](const Entity& e) {
                return kind(e) == EntityKind::Dimension || kind(e) == EntityKind::Leader ||
                       kind(e) == EntityKind::MLeader;
            }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Style", "Scale"};
        ko.defaultKeyword = "Scale";
        auto k = co_await ctx.getKeyword("Set [Style/Scale] <Scale>", ko);
        if (!k.ok())
            co_return;
        cmd::StringOptions so;
        auto v =
            co_await ctx.getString(k.keyword == "Style" ? "Style name" : "Scale as N of 1:N", so);
        if (!v.ok())
            co_return;
        const std::string text = cmd::trim(v.text);
        std::size_t n = 0;
        if (k.keyword == "Style") {
            const DimStyle* st = ctx.doc().dimStyleByName(text);
            if (!st) {
                ctx.message("No dimension style named \"" + text + "\".");
                co_return;
            }
            const DimStyleId id = st->id;
            for (EntityId e : sel.ids)
                n += assign(ctx, e, [&](DimStyleId) { return id; });
        } else {
            double scale = 0;
            if (!parseReal(text, scale) || !(scale > 0)) {
                ctx.message("The scale must be a number above zero.");
                co_return;
            }
            char tag[40];
            std::snprintf(tag, sizeof tag, "-1to%g", scale);
            for (EntityId e : sel.ids)
                n += assign(ctx, e, [&](DimStyleId current) {
                    const DimStyle base = dim::styleOf(ctx.doc(), current);
                    if (base.overallScale == scale)
                        return base.id;
                    // Derive from the original style, not from an earlier "-1toN" copy.
                    std::string root = base.name;
                    if (auto at = root.rfind("-1to"); at != std::string::npos && at > 0)
                        root.resize(at);
                    const std::string name = root + tag;
                    if (const DimStyle* have = ctx.doc().dimStyleByName(name))
                        return have->id;
                    DimStyle copy = base;
                    copy.id = {};
                    copy.name = name;
                    copy.overallScale = scale;
                    auto id = ctx.tx().addDimStyle(std::move(copy));
                    return id ? id.value() : current;
                });
        }
        ctx.message(std::to_string(n) + " changed");
        ctx.setUndoLabel("Dimension setup");
    }

private:
    template <class F>
    static std::size_t assign(cmd::CommandContext& ctx, EntityId e, F pick) {
        const Entity* ent = ctx.doc().find(e);
        if (!ent)
            return 0;
        DimStyleId current{};
        if (const auto* d = entityAs<DimensionData>(*ent))
            current = d->style;
        else if (const auto* l = entityAs<LeaderData>(*ent))
            current = l->style;
        else if (const auto* m = entityAs<MLeaderData>(*ent))
            current = m->style;
        else
            return 0;
        if (!current.value)
            current = ctx.doc().header().currentDimStyle;
        const DimStyleId target = pick(current);
        if (target == current)
            return 0;
        auto r = ctx.tx().modify(e, [&](Entity& x) {
            if (auto* d = entityAs<DimensionData>(x))
                d->style = target;
            else if (auto* l = entityAs<LeaderData>(x))
                l->style = target;
            else if (auto* m = entityAs<MLeaderData>(x))
                m->style = target;
        });
        return r ? 1 : 0;
    }
};

// DIMOVERRIDE: changes one property of the selected dimensions only. Each gets a style of its
// own, derived from the one it used ("<style>$N"); equal overrides share a style. "Clear" puts the
// dimensions back on the style they were derived from.
class DimOverrideCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMOVERRIDE"; }
    std::string_view description() const override {
        return "Override a dimension style property on the selected dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select dimensions",
                                             cmd::SelectionOptions{.filter = [](const Entity& e) {
                                                 return kind(e) == EntityKind::Dimension;
                                             }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = propertyKeywords();
        ko.keywords.push_back("Clear");
        auto p = co_await ctx.getKeyword("Property to override or [Clear]", ko);
        if (!p.ok())
            co_return;
        std::string value;
        if (p.keyword != "Clear") {
            cmd::StringOptions vo;
            auto v = co_await ctx.getString(p.keyword + " value", vo);
            if (!v.ok())
                co_return;
            value = cmd::trim(v.text);
            // Check the value once on a scratch copy before touching anything.
            DimStyle probe = dim::styleOf(ctx.doc(), ctx.doc().header().currentDimStyle);
            const std::string err = applyProperty(ctx.doc(), probe, p.keyword, value);
            if (!err.empty()) {
                ctx.message(err);
                co_return;
            }
        }
        std::size_t changed = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            const auto* d = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d)
                continue;
            const DimStyle base = dim::styleOf(ctx.doc(), d->style);
            DimStyleId target = base.id;
            std::string root = base.name;
            if (auto at = root.rfind('$'); at != std::string::npos && at > 0)
                root.resize(at);
            if (p.keyword == "Clear") {
                if (const DimStyle* r = ctx.doc().dimStyleByName(root))
                    target = r->id;
            } else {
                DimStyle derived = base;
                if (!applyProperty(ctx.doc(), derived, p.keyword, value).empty())
                    continue;
                if (describe(ctx.doc(), derived) == describe(ctx.doc(), base))
                    continue;
                // Reuse an existing override with the same settings.
                target = {};
                for (const auto& [sid, st] : ctx.doc().dimStyles()) {
                    if (st.name.rfind(root + "$", 0) == 0 &&
                        describe(ctx.doc(), st) == describe(ctx.doc(), derived))
                        target = sid;
                }
                if (!target.value) {
                    int n = 1;
                    while (ctx.doc().dimStyleByName(root + "$" + std::to_string(n)))
                        ++n;
                    derived.id = {};
                    derived.name = root + "$" + std::to_string(n);
                    auto added = ctx.tx().addDimStyle(std::move(derived));
                    if (!added)
                        continue;
                    target = added.value();
                }
            }
            if (target == base.id && d->style.value)
                continue;
            auto r = ctx.tx().modify(id, [&](Entity& x) {
                if (auto* dd = entityAs<DimensionData>(x)) {
                    dd->style = target;
                    dim::refresh(ctx.doc(), *dd);
                }
            });
            changed += r ? 1 : 0;
        }
        ctx.message(std::to_string(changed) + " changed");
        ctx.setUndoLabel("Dimension override");
    }
};

class DimStylesPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMSTYLES"; }
    std::string_view description() const override { return "Open the dimension style manager"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("dimstyles");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeDimStylesPanelCommand() {
    return std::make_unique<DimStylesPanelCommand>();
}

std::unique_ptr<cmd::Command> makeDimOverrideCommand() {
    return std::make_unique<DimOverrideCommand>();
}

std::unique_ptr<cmd::Command> makeDimSetupCommand() {
    return std::make_unique<DimSetupCommand>();
}

std::unique_ptr<cmd::Command> makeDimStyleCommand() {
    return std::make_unique<DimStyleCommand>();
}

}  // namespace tbcad::dimcmd
