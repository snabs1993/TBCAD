#include "common.h"

namespace tbcad::select::detail {

namespace {

constexpr const char* kKey = "matchprop.settings";
constexpr unsigned kLtScale = 32;
constexpr unsigned kAll = 31 | kLtScale;

struct Name {
    const char* name;
    unsigned bit;
};
const Name kNames[] = {{"Layer", static_cast<unsigned>(Match::Layer)},
                       {"Color", static_cast<unsigned>(Match::Color)},
                       {"Linetype", static_cast<unsigned>(Match::Linetype)},
                       {"Lineweight", static_cast<unsigned>(Match::Lineweight)},
                       {"Scale", kLtScale}};

unsigned load(cmd::CommandContext& ctx) {
    if (auto s = ctx.host().setting(kKey)) {
        try {
            return static_cast<unsigned>(std::stoul(*s)) & kAll;
        } catch (...) {
        }
    }
    return kAll & ~static_cast<unsigned>(Match::Type);
}

std::string describe(unsigned m) {
    std::string s;
    for (const auto& n : kNames)
        if (m & n.bit)
            s += (s.empty() ? "" : ", ") + std::string(n.name);
    return s.empty() ? "nothing" : s;
}

// MATCHPROP [MA]: copy layer, colour, linetype, lineweight and linetype scale from one object to
// others. Settings picks which of them are copied.
class MatchProp final : public cmd::Command {
public:
    std::string_view name() const override { return "MATCHPROP"; }
    std::vector<std::string> defaultAliases() const override { return {"MA"}; }
    std::string_view description() const override {
        return "Copy the properties of one object to other objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        auto src = co_await ctx.getEntity("Select source object");
        if (!src.ok())
            co_return;
        const Entity* source = ctx.doc().find(src.id);
        if (!source)
            co_return;
        const EntityCommon from = source->common;
        const EntityId sourceId = src.id;
        int count = 0;
        for (;;) {
            unsigned m = load(ctx);
            cmd::EntityOptions eo;
            eo.keywords = {"Settings"};
            auto d = co_await ctx.getEntity(
                "Select destination object or [Settings] (" + describe(m) + ")", std::move(eo));
            if (d.status == cmd::Status::Keyword) {
                co_await settings(ctx);
                continue;
            }
            if (!d.ok())
                break;
            if (d.id == sourceId || !ctx.doc().find(d.id))
                continue;
            ctx.checkpoint();
            auto r = ctx.tx().modify(d.id, [&](Entity& x) {
                if (m & static_cast<unsigned>(Match::Layer))
                    x.common.layer = from.layer;
                if (m & static_cast<unsigned>(Match::Color))
                    x.common.color = from.color;
                if (m & static_cast<unsigned>(Match::Linetype))
                    x.common.linetype = from.linetype;
                if (m & static_cast<unsigned>(Match::Lineweight))
                    x.common.lineweight = from.lineweight;
                if (m & kLtScale)
                    x.common.linetypeScale = from.linetypeScale;
            });
            if (r)
                ++count;
        }
        if (count)
            ctx.message(std::to_string(count) + " object(s) changed.");
    }

private:
    static cmd::Task settings(cmd::CommandContext& ctx) {
        for (;;) {
            unsigned m = load(ctx);
            cmd::KeywordOptions ko;
            ko.keywords = {"Layer", "Color", "Linetype", "Lineweight", "Scale", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword(
                "Copy: " + describe(m) +
                    ". Toggle [Layer/Color/Linetype/Lineweight/Scale/Done] <Done>",
                std::move(ko));
            if (!k.ok() || k.keyword == "Done")
                co_return;
            for (const auto& n : kNames)
                if (k.keyword == n.name)
                    m ^= n.bit;
            ctx.host().setSetting(kKey, std::to_string(m));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMatchProp() {
    return std::make_unique<MatchProp>();
}

}  // namespace tbcad::select::detail
