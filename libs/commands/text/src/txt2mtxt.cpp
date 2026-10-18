// TXT2MTXT: merges single-line and multiline text into one multiline text, top to bottom.
#include <algorithm>
#include <cmath>

#include "commands.h"

namespace tbcad::textcmd {

namespace {

std::string escaped(const std::string& plain) {
    std::string out;
    for (char c : plain) {
        if (c == '\\' || c == '{' || c == '}')
            out += '\\';
        out += c;
    }
    return out;
}

class Txt2MTxtCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TXT2MTXT"; }
    std::string_view description() const override {
        return "Merge text objects into one multiline text";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            const auto* t = entityAs<TextData>(e);
            return (t && !t->attdef) || entityAs<MTextData>(e) != nullptr;
        };
        auto sel = co_await ctx.getSelection("Select text objects to merge", so);
        if (!sel.ok())
            co_return;

        struct Item {
            EntityId id;
            Vec2 pos;
            double height, rotation;
            TextStyleId style;
            std::string markup;
        };
        std::vector<Item> items;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            if (const auto* t = entityAs<TextData>(*e))
                items.push_back(
                    {id, t->position, t->height, t->rotation, t->style, escaped(t->utf8)});
            else if (const auto* m = entityAs<MTextData>(*e))
                items.push_back({id, m->position, m->height, m->rotation, m->style, m->utf8});
        }
        if (items.size() < 2) {
            ctx.message("Select at least two text objects.");
            co_return;
        }
        const double rot = items[0].rotation;
        const Vec2 up{-std::sin(rot), std::cos(rot)};
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            return dot(a.pos, up) > dot(b.pos, up);
        });

        MTextData m;
        m.attach = Attachment::TopLeft;
        m.height = items[0].height;
        m.rotation = rot;
        m.style = items[0].style;
        m.position = items[0].pos + up * items[0].height;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i)
                m.utf8 += "\\P";
            m.utf8 += items[i].markup;
        }
        Entity e;
        e.data = Indirect<MTextData>(std::move(m));
        if (const Entity* first = ctx.doc().find(items[0].id))
            e.common = first->common;
        e.common.id = EntityId{};
        ctx.setUndoLabel("TXT2MTXT");
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        for (const Item& it : items)
            (void)ctx.tx().erase(it.id);
        ctx.host().setSelection({id.value()});
        ctx.message(std::to_string(items.size()) + " text objects merged.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTxt2MTxtCommand() {
    return std::make_unique<Txt2MTxtCommand>();
}

}  // namespace tbcad::textcmd
