// TXT2MTXT: merges single-line and multiline text into one multiline text. As in AutoCAD,
// SEttings choose the order (top down or the order picked) and word wrap (one paragraph as wide as
// the widest text, instead of one line per text).
#include <algorithm>
#include <cmath>

#include "commands.h"
#include "tbcad/geom/entity.h"

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
        so.keywords = {"SEttings"};
        cmd::SelectionResult sel;
        for (;;) {
            sel = co_await ctx.getSelection("Select text objects to merge or [SEttings]", so);
            if (sel.status != cmd::Status::Keyword)
                break;
            bool ok = false;
            co_await settings(ctx, ok);
            if (!ok)
                co_return;
        }
        if (!sel.ok())
            co_return;
        const bool topDown = ctx.host().setting("txt2mtxt.order").value_or("top") != "select";
        const bool wrap = ctx.host().setting("txt2mtxt.wrap").value_or("0") == "1";

        struct Item {
            EntityId id;
            Vec2 pos;
            double height, rotation;
            TextStyleId style;
            std::string markup;
            double width;
        };
        std::vector<Item> items;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            const BBox2 b = geom::bounds(*e, ctx.doc());
            const double w = b.empty() ? 0.0 : b.max.x - b.min.x;
            if (const auto* t = entityAs<TextData>(*e))
                items.push_back(
                    {id, t->position, t->height, t->rotation, t->style, escaped(t->utf8), w});
            else if (const auto* m = entityAs<MTextData>(*e))
                items.push_back({id, m->position, m->height, m->rotation, m->style, m->utf8, w});
        }
        if (items.size() < 2) {
            ctx.message("Select at least two text objects.");
            co_return;
        }
        const double rot = items[0].rotation;
        const Vec2 up{-std::sin(rot), std::cos(rot)};
        if (topDown)
            std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
                return dot(a.pos, up) > dot(b.pos, up);
            });
        // The new text starts at the top line, whatever the order of the words.
        const Item* topItem = &items[0];
        for (const Item& it : items)
            if (dot(it.pos, up) > dot(topItem->pos, up))
                topItem = &it;

        MTextData m;
        m.attach = Attachment::TopLeft;
        m.height = items[0].height;
        m.rotation = rot;
        m.style = items[0].style;
        m.position = topItem->pos + up * topItem->height;
        double widest = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i)
                m.utf8 += wrap ? " " : "\\P";
            m.utf8 += items[i].markup;
            widest = std::max(widest, items[i].width);
        }
        if (wrap)
            m.width = widest;  // one paragraph, wrapped at the widest text
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

private:
    // SEttings: [Order/Wrap/eXit]; the choices are remembered. `ok` is false when cancelled.
    static cmd::Task settings(cmd::CommandContext& ctx, bool& ok) {
        ok = false;
        for (;;) {
            const bool select = ctx.host().setting("txt2mtxt.order").value_or("top") == "select";
            const bool wrap = ctx.host().setting("txt2mtxt.wrap").value_or("0") == "1";
            ctx.message(std::string("Order: ") + (select ? "as selected" : "top down") +
                        ", word wrap: " + (wrap ? "on" : "off") + ".");
            cmd::KeywordOptions ko;
            ko.keywords = {"Order", "Wrap", "eXit"};
            ko.defaultKeyword = "eXit";
            auto k = co_await ctx.getKeyword("Settings [Order/Wrap/eXit] <eXit>", ko);
            if (!k.ok())
                co_return;
            if (k.keyword == "eXit") {
                ok = true;
                co_return;
            }
            if (k.keyword == "Order") {
                cmd::KeywordOptions oo;
                oo.keywords = {"Top", "Select"};
                oo.defaultKeyword = select ? "Select" : "Top";
                auto o = co_await ctx.getKeyword("Order [Top down/Select order]", oo);
                if (!o.ok())
                    co_return;
                ctx.host().setSetting("txt2mtxt.order", o.keyword == "Select" ? "select" : "top");
            } else {
                cmd::KeywordOptions wo;
                wo.keywords = {"Yes", "No"};
                wo.defaultKeyword = wrap ? "Yes" : "No";
                auto w = co_await ctx.getKeyword("Word wrap [Yes/No]", wo);
                if (!w.ok())
                    co_return;
                ctx.host().setSetting("txt2mtxt.wrap", w.keyword == "Yes" ? "1" : "0");
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTxt2MTxtCommand() {
    return std::make_unique<Txt2MTxtCommand>();
}

}  // namespace tbcad::textcmd
