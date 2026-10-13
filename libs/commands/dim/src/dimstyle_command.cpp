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
        return "Create, select, edit or delete dimension styles";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();
        ctx.setUndoLabel("Dimension style");
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Current", "New", "Delete", "Edit", "List", "Done"};
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
            } else if (k.keyword == "Delete") {
                const DimStyle* s = ctx.doc().dimStyleByName(name);
                if (!s)
                    ctx.message("No dimension style named \"" + name + "\".");
                else if (s->id == ctx.doc().header().currentDimStyle)
                    ctx.message("The current style cannot be deleted; make another one current.");
                else
                    check(ctx, ctx.tx().eraseDimStyle(s->id));
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

}  // namespace

std::unique_ptr<cmd::Command> makeDimStyleCommand() {
    return std::make_unique<DimStyleCommand>();
}

}  // namespace tbcad::dimcmd
