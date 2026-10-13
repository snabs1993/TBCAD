#include <cmath>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

constexpr std::size_t kPreviewCopies = 400;  // the preview stops here; the result is not limited

// Draws copies of `source` at the given transforms (at most kPreviewCopies).
void previewCopies(const Document& doc, const std::vector<Entity>& source,
                   const std::vector<Transform2>& ts, double pixelSize, DisplayList& out) {
    geom::DisplayContext dc;
    dc.pixelSizeMm = pixelSize;
    std::size_t n = 0;
    for (const Transform2& t : ts) {
        if (++n > kPreviewCopies)
            break;
        for (const Entity& e : source)
            if (auto moved = geom::transformed(e, t))
                geom::buildDisplay(doc, moved.value(), dc, out);
    }
}

// ARRAY [AR]: rectangular or polar copies of the selected objects. Plain copies, not associative.
class ArrayCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ARRAY"; }
    std::vector<std::string> defaultAliases() const override { return {"AR"}; }
    std::string_view description() const override {
        return "Copy objects in a rectangular or polar pattern";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto sel =
            co_await ctx.getSelection("Select objects to array",
                                      cmd::SelectionOptions{.filter = detail::editableFilter(doc)});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::vector<Entity> source;
        for (EntityId id : detail::inDrawOrder(doc, sel.ids))
            source.push_back(*doc.find(id));
        if (source.empty())
            co_return;
        const BBox2 box = detail::boundsOf(doc, source);

        auto type =
            co_await ctx.getKeyword("Array type [Rectangular/Polar] <Rectangular>",
                                    cmd::KeywordOptions{.keywords = {"Rectangular", "Polar"},
                                                        .defaultKeyword = "Rectangular"});
        if (!type.ok())
            co_return;

        std::vector<Entity> copies;
        if (type.keyword == "Polar") {
            auto center = co_await ctx.getPoint("Specify center point of array");
            if (!center.ok())
                co_return;
            auto count = co_await ctx.getInteger(
                "Enter the number of items in the array (including the original) <6>",
                cmd::IntOptions{.defaultValue = 6, .min = 2, .max = blocks::kMaxArrayItems});
            if (!count.ok())
                co_return;
            const double pixel = ctx.host().view().scale;
            const Vec2 c = center.point;
            const int n = static_cast<int>(count.value);
            auto preview = [&doc, source, c, n, pixel](Vec2 cursor, DisplayList& out) {
                blocks::PolarArray spec{c, n, 0, true, std::nullopt};
                double a = std::atan2(cursor.y - c.y, cursor.x - c.x);
                spec.fillAngle = a > 0 ? a : a + kTwoPi;
                previewCopies(doc, source,
                              blocks::polarTransforms(spec, detail::boundsOf(doc, source).center()),
                              pixel, out);
            };
            auto fill = co_await ctx.getAngle(
                "Specify the angle to fill (+ = counterclockwise, - = clockwise) <360>",
                cmd::AngleOptions{.base = c, .preview = preview, .defaultValue = kTwoPi});
            if (!fill.ok())
                co_return;
            auto rot = co_await ctx.getKeyword(
                "Rotate items as copied? [Yes/No] <Yes>",
                cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "Yes"});
            if (!rot.ok())
                co_return;
            blocks::PolarArray spec{c, n, fill.value, rot.keyword == "Yes", std::nullopt};
            auto r = blocks::arrayPolar(source, spec);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            copies = std::move(r).value();
        } else {
            auto rows = co_await ctx.getInteger(
                "Enter the number of rows <3>",
                cmd::IntOptions{.defaultValue = 3, .min = 1, .max = blocks::kMaxArrayItems});
            if (!rows.ok())
                co_return;
            auto cols = co_await ctx.getInteger(
                "Enter the number of columns <4>",
                cmd::IntOptions{.defaultValue = 4, .min = 1, .max = blocks::kMaxArrayItems});
            if (!cols.ok())
                co_return;
            const double w = box.empty() || box.width() <= 0 ? 10.0 : box.width() * 1.5;
            const double h = box.empty() || box.height() <= 0 ? 10.0 : box.height() * 1.5;
            const double pixel = ctx.host().view().scale;
            blocks::RectArray spec;
            spec.rows = static_cast<int>(rows.value);
            spec.columns = static_cast<int>(cols.value);
            auto colSp = co_await ctx.getDistance(
                "Specify the distance between columns <" + std::to_string(w) + ">",
                cmd::DistanceOptions{.defaultValue = w, .allowZero = false, .allowNegative = true});
            if (!colSp.ok())
                co_return;
            spec.columnSpacing = colSp.value;
            const Vec2 anchor = box.empty() ? Vec2{} : box.min;
            auto preview = [&doc, source, spec, anchor, pixel](Vec2 cursor, DisplayList& out) {
                blocks::RectArray s = spec;
                s.rowSpacing = distance(cursor, anchor);
                previewCopies(doc, source, blocks::rectangularTransforms(s), pixel, out);
            };
            auto rowSp = co_await ctx.getDistance(
                "Specify the distance between rows <" + std::to_string(h) + ">",
                cmd::DistanceOptions{.base = anchor,
                                     .preview = preview,
                                     .defaultValue = h,
                                     .allowZero = false,
                                     .allowNegative = true});
            if (!rowSp.ok())
                co_return;
            spec.rowSpacing = rowSp.value;
            auto ang = co_await ctx.getAngle("Specify the angle of the array <0>",
                                             cmd::AngleOptions{.defaultValue = 0.0});
            if (!ang.ok())
                co_return;
            spec.angle = ang.value;
            auto r = blocks::arrayRectangular(source, spec);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            copies = std::move(r).value();
        }

        std::vector<EntityId> created;
        for (Entity& e : copies) {
            auto added = ctx.tx().add(std::move(e));
            if (!added) {
                ctx.message(added.error().message);
                co_return;  // rolls the whole array back
            }
            created.push_back(added.value());
        }
        ctx.host().setSelection(created);
        ctx.message("Created " + detail::objects(created.size()));
        ctx.setUndoLabel("Array " + detail::objects(created.size()));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeArrayCommand() {
    return std::make_unique<ArrayCommand>();
}

}  // namespace tbcad::block_cmds
