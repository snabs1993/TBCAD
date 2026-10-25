#include <cmath>
#include <cstdio>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/text/field.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Task;

// FIELD: a text that works out its own content each time it is drawn (date, file name, sheet
// number, length or area of an object, the text of another text). Edit the text afterwards like
// any other; the part between %< and >% is the field.
class FieldCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FIELD"; }
    std::vector<std::string> defaultAliases() const override { return {"FLD", "RTEXT"}; }
    std::string_view description() const override {
        return "Text that updates itself: date, file name, sheet, length, area, layer, block, ...";
    }

    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Date",  "Filename", "Sheet",     "Length", "Area",  "Text",
                       "Layer", "Block",    "Attribute", "Radius", "Scale", "SetSheet"};
        ko.defaultKeyword = "Date";
        auto k = co_await ctx.getKeyword(
            "Field [Date/Filename/Sheet/Length/Area/Text/Layer/Block/Attribute/Radius/Scale/"
            "SetSheet] <Date>",
            ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "SetSheet") {
            cmd::StringOptions so;
            auto s = co_await ctx.getString("Sheet number", so);
            if (!s.ok())
                co_return;
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom["sheet.number"] = s.text; });
            ctx.setUndoLabel("Sheet number");
            co_return;
        }

        std::string token;
        if (k.keyword == "Date") {
            token = "%<date>%";
        } else if (k.keyword == "Filename") {
            token = "%<filename>%";
        } else if (k.keyword == "Sheet") {
            token = "%<sheet>%";
        } else if (k.keyword == "Scale") {
            token = "%<scale>%";
        } else if (k.keyword == "Layer" || k.keyword == "Block" || k.keyword == "Attribute" ||
                   k.keyword == "Radius") {
            const std::string which = k.keyword;
            cmd::EntityOptions eo;
            eo.filter = [which](const Entity& e) {
                if (which == "Layer")
                    return true;
                if (which == "Radius")
                    return kind(e) == EntityKind::Circle || kind(e) == EntityKind::Arc;
                return kind(e) == EntityKind::Insert;
            };
            auto o = co_await ctx.getEntity("Select the object", eo);
            if (!o.ok())
                co_return;
            const std::string id = "#" + std::to_string(o.id.value);
            if (which == "Layer") {
                token = "%<layer:" + id + ">%";
            } else if (which == "Block") {
                token = "%<block:" + id + ">%";
            } else if (which == "Radius") {
                token = "%<radius:" + id + ":2>%";
            } else {
                auto tag = co_await ctx.getString("Attribute tag");
                if (!tag.ok())
                    co_return;
                token = "%<attr:" + id + ":" + tag.text + ">%";
            }
        } else {
            cmd::EntityOptions eo;
            const bool text = k.keyword == "Text";
            const bool area = k.keyword == "Area";
            eo.filter = [text, area](const Entity& e) {
                switch (kind(e)) {
                    case EntityKind::Text:
                    case EntityKind::MText:
                        return text;
                    case EntityKind::Circle:
                        return !text;
                    case EntityKind::Polyline:
                        return !text;
                    case EntityKind::Hatch:
                        return area;
                    case EntityKind::Ellipse:
                        return !text;
                    case EntityKind::Line:
                    case EntityKind::Arc:
                        return !text && !area;
                    default:
                        return false;
                }
            };
            auto o = co_await ctx.getEntity("Select the object", eo);
            if (!o.ok())
                co_return;
            const std::string id = "#" + std::to_string(o.id.value);
            token = text ? "%<text:" + id + ">%"
                         : std::string("%<") + (area ? "area:" : "length:") + id + ":2>%";
        }

        cmd::PointOptions po;
        auto p = co_await ctx.getPoint("Specify where to put the field", po);
        if (!p.ok())
            co_return;
        TextData d;
        d.position = p.point;
        d.utf8 = token;
        Entity e{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
        else
            ctx.setUndoLabel("Field");
    }
};

// FONTALT: the font that stands in for a missing .shx font. FONTMAP: a font mapping file
// ("old;new" per line) that says which font replaces which.
class FontSubstitutionCommand final : public cmd::Command {
public:
    explicit FontSubstitutionCommand(bool map) : map_(map) {}
    std::string_view name() const override { return map_ ? "FONTMAP" : "FONTALT"; }
    std::string_view description() const override {
        return map_ ? "Set the font mapping file that replaces fonts (old;new per line)"
                    : "Set the font that replaces a missing .shx font";
    }

    Task run(CommandContext& ctx) override {
        const char* key = map_ ? "text.fontmap.file" : "text.fontalt";
        cmd::StringOptions so;
        so.allowEmpty = true;
        so.defaultValue = ctx.host().setting(key).value_or("");
        auto v = co_await ctx.getString(
            map_ ? "Font mapping file, . for none" : "Alternate font, . for none", so);
        if (!v.ok() && v.status != cmd::Status::None)
            co_return;
        std::string value = cmd::trim(v.text);
        if (value.empty())
            value = so.defaultValue.value_or("");
        if (value == ".")
            value.clear();
        ctx.host().setSetting(key, value);
        std::string error;
        text::setFontSubstitutionsFromFile(ctx.host().setting("text.fontmap.file").value_or(""),
                                           ctx.host().setting("text.fontalt").value_or(""), &error);
        ctx.message(error.empty() ? std::string(name()) + " = " + (value.empty() ? "." : value)
                                  : error);
    }

private:
    bool map_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeFontAltCommand() {
    return std::make_unique<FontSubstitutionCommand>(false);
}

std::unique_ptr<cmd::Command> makeFontMapCommand() {
    return std::make_unique<FontSubstitutionCommand>(true);
}

std::unique_ptr<cmd::Command> makeFieldCommand() {
    return std::make_unique<FieldCommand>();
}

}  // namespace tbcad::textcmd
