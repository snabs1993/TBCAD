#include <algorithm>

#include "common.h"

namespace tbcad::select::detail {

namespace {

constexpr const char* kMatchKey = "selectsimilar.match";
constexpr unsigned kDefaultMatch =
    static_cast<unsigned>(Match::Type) | static_cast<unsigned>(Match::Layer);

struct MatchName {
    const char* name;
    Match bit;
};
const MatchName kNames[] = {{"Type", Match::Type},
                            {"Layer", Match::Layer},
                            {"Color", Match::Color},
                            {"Linetype", Match::Linetype},
                            {"Lineweight", Match::Lineweight},
                            {"Level", Match::Level},
                            {"Transparency", Match::Transparency},
                            {"Style", Match::Style},
                            {"Size", Match::Size}};

unsigned loadMatch(cmd::CommandContext& ctx) {
    if (auto s = ctx.host().setting(kMatchKey)) {
        try {
            return static_cast<unsigned>(std::stoul(*s)) & 511u;
        } catch (...) {
        }
    }
    return kDefaultMatch;
}

std::string describe(unsigned m) {
    std::string s;
    for (const auto& n : kNames)
        if (m & static_cast<unsigned>(n.bit))
            s += (s.empty() ? "" : ", ") + std::string(n.name);
    return s.empty() ? "nothing" : s;
}

// SELECTSIMILAR [SELSIM]: pick an object and select every shown object that agrees with it on the
// chosen properties (type and layer unless Settings says otherwise).
class SelectSimilar final : public cmd::Command {
public:
    std::string_view name() const override { return "SELECTSIMILAR"; }
    std::vector<std::string> defaultAliases() const override { return {"SELSIM"}; }
    std::string_view description() const override {
        return "Select objects that match the properties of a picked object";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            unsigned match = loadMatch(ctx);
            EntityId ref{};
            if (!ctx.host().selection().empty() && ctx.doc().find(ctx.host().selection().front())) {
                ref = ctx.host().selection().front();
            } else {
                cmd::EntityOptions eo;
                eo.keywords = {"Settings"};
                auto e = co_await ctx.getEntity("Select an object or [Settings]", std::move(eo));
                if (e.status == cmd::Status::Keyword) {
                    co_await settings(ctx);
                    continue;
                }
                if (!e.ok())
                    co_return;
                ref = e.id;
            }
            const Entity* ent = ctx.doc().find(ref);
            if (!ent)
                co_return;
            auto ids = similarEntities(ctx.doc(), *ent, match);
            ctx.host().setSelection(ids);
            ctx.message(std::to_string(ids.size()) + " object(s) found matching " +
                        describe(match) + ".");
            co_return;
        }
    }

private:
    static cmd::Task settings(cmd::CommandContext& ctx) {
        for (;;) {
            unsigned match = loadMatch(ctx);
            cmd::KeywordOptions ko;
            ko.keywords = {"Type",  "Layer",        "Color", "Linetype", "Lineweight",
                           "Level", "Transparency", "Style", "Size",     "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Match on: " + describe(match) +
                                                 ". Toggle "
                                                 "[Type/Layer/Color/Linetype/Lineweight/Level/"
                                                 "Transparency/Style/Size/Done] <Done>",
                                             std::move(ko));
            if (!k.ok() || k.keyword == "Done")
                co_return;
            for (const auto& n : kNames)
                if (k.keyword == n.name)
                    match ^= static_cast<unsigned>(n.bit);
            ctx.host().setSetting(kMatchKey, std::to_string(match));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSelectSimilar() {
    return std::make_unique<SelectSimilar>();
}

}  // namespace tbcad::select::detail
