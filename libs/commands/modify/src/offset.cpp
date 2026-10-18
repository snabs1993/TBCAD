#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

constexpr const char* kDistanceKey = "offset.distance";
constexpr const char* kRoundKey = "offset.roundcorners";
constexpr const char* kLayerKey = "offset.currentlayer";
constexpr const char* kEraseKey = "offset.erasesource";

modify::OffsetOptions optionsFrom(const cmd::IHost& host) {
    modify::OffsetOptions o;
    o.roundCorners = getFlag(host, kRoundKey, false);
    return o;
}

class OffsetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "OFFSET"; }
    std::vector<std::string> defaultAliases() const override { return {"O"}; }
    std::string_view description() const override {
        return "Create a parallel copy of a line, arc, circle, polyline, ellipse or spline";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        double dist = getSetting(ctx.host(), kDistanceKey, 0);
        bool through = false;
        for (;;) {
            cmd::DistanceOptions d;
            d.keywords = {"Through", "Erase", "Corners", "Layer"};
            d.allowZero = false;
            if (dist > 0)
                d.defaultValue = dist;
            std::string prompt = "Specify offset distance or [Through/Erase/Corners/Layer]";
            auto r = co_await ctx.getDistance(prompt, d);
            if (r.status == cmd::Status::Keyword && r.keyword == "Corners") {
                cmd::KeywordOptions k;
                k.keywords = {"Round", "Sharp"};
                k.defaultKeyword = getFlag(ctx.host(), kRoundKey, false) ? "Round" : "Sharp";
                auto c = co_await ctx.getKeyword("Corner type at polyline bends [Round/Sharp]", k);
                if (c.ok())
                    putFlag(ctx.host(), kRoundKey, c.keyword == "Round");
                continue;
            }
            if (r.status == cmd::Status::Keyword && r.keyword == "Erase") {
                cmd::KeywordOptions k;
                k.keywords = {"Yes", "No"};
                k.defaultKeyword = getFlag(ctx.host(), kEraseKey, false) ? "Yes" : "No";
                auto c =
                    co_await ctx.getKeyword("Erase source object after offsetting [Yes/No]", k);
                if (c.ok())
                    putFlag(ctx.host(), kEraseKey, c.keyword == "Yes");
                continue;
            }
            if (r.status == cmd::Status::Keyword && r.keyword == "Layer") {
                cmd::KeywordOptions k;
                k.keywords = {"Current", "Source"};
                k.defaultKeyword = getFlag(ctx.host(), kLayerKey, false) ? "Current" : "Source";
                auto c = co_await ctx.getKeyword("Layer of the new objects [Current/Source]", k);
                if (c.ok())
                    putFlag(ctx.host(), kLayerKey, c.keyword == "Current");
                continue;
            }
            if (r.status == cmd::Status::Keyword) {
                through = true;
            } else if (r.ok()) {
                dist = r.value;
                putSetting(ctx.host(), kDistanceKey, dist);
            } else {
                co_return;
            }
            break;
        }
        const modify::OffsetOptions opt = optionsFrom(ctx.host());

        for (;;) {
            cmd::EntityOptions eo;
            eo.filter = [&doc = ctx.doc()](const Entity& e) { return isTrimTarget(doc, e); };
            auto pick = co_await ctx.getEntity("Select object to offset or <exit>", eo);
            if (!pick.ok())
                co_return;
            const Entity* found = ctx.doc().find(pick.id);
            if (!found)
                continue;
            Entity source = *found;

            // "Multiple" keeps offsetting the same object, one side point after the other, until
            // Enter.
            bool multiple = false;
            for (;;) {
                cmd::PointOptions po;
                po.keywords = {"Multiple"};
                po.allowNone = multiple;
                po.preview = [source, through, dist, opt, &doc = ctx.doc(), &host = ctx.host()](
                                 Vec2 cursor, DisplayList& out) {
                    auto res = through ? modify::offsetThrough(source, cursor, opt)
                                       : modify::offsetToward(source, cursor, dist, opt);
                    if (!res)
                        return;
                    for (const Entity& e : res.value())
                        previewEntity(doc, host, e, out);
                };
                auto side = co_await ctx.getPoint(
                    through ? (multiple ? "Specify through point or <next object>"
                                        : "Specify through point or [Multiple]")
                            : (multiple ? "Specify point on side to offset or <next object>"
                                        : "Specify point on side to offset or [Multiple]"),
                    po);
                if (side.status == cmd::Status::Cancel)
                    co_return;
                if (side.status == cmd::Status::Keyword && side.keyword == "Multiple") {
                    multiple = true;
                    continue;
                }
                if (side.status == cmd::Status::None && multiple)
                    break;
                if (!side.ok())
                    break;
                auto res = through ? modify::offsetThrough(source, side.point, opt)
                                   : modify::offsetToward(source, side.point, dist, opt);
                if (!res) {
                    ctx.message(res.error().message);
                    if (!multiple)
                        break;
                    continue;
                }
                const bool onCurrent = getFlag(ctx.host(), kLayerKey, false);
                for (Entity& e : res.value()) {
                    if (onCurrent)
                        e.common.layer = ctx.doc().header().currentLayer;
                    auto id = ctx.tx().add(std::move(e));
                    if (!id)
                        ctx.message(id.error().message);
                }
                if (getFlag(ctx.host(), kEraseKey, false)) {
                    (void)ctx.tx().erase(pick.id);
                    break;
                }
                if (!multiple)
                    break;
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeOffset() {
    return std::make_unique<OffsetCommand>();
}

}  // namespace tbcad::modifycmd
