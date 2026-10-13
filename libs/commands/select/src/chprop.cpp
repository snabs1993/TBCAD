#include <cmath>

#include "common.h"

namespace tbcad::select::detail {

namespace {

// A layer by name; "Parent/Child" walks the sub-layer path.
const Layer* findLayer(const Document& doc, const std::string& path) {
    const Layer* cur = nullptr;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? slash : slash - start);
        cur = cur ? doc.layerByName(cur->id, part) : doc.layerByName(part);
        if (!cur || slash == std::string::npos)
            return cur;
        start = slash + 1;
    }
}

// CHPROP [CH]: change the layer, colour, linetype, lineweight or linetype scale of the selected
// objects. Works on the current selection without asking, so panels can run it as a command line
// ("CHPROP Color 1").
class ChProp final : public cmd::Command {
public:
    std::string_view name() const override { return "CHPROP"; }
    std::vector<std::string> defaultAliases() const override { return {"CH"}; }
    std::string_view description() const override {
        return "Change the layer, colour, linetype or lineweight of objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Layer", "Color", "Linetype", "Lineweight", "Scale"};
            auto k = co_await ctx.getKeyword(
                "Property to change [Layer/Color/Linetype/Lineweight/Scale]", std::move(ko));
            if (!k.ok())
                co_return;
            auto v = co_await ctx.getString("New " + k.keyword + " value");
            if (!v.ok())
                co_return;
            std::function<void(EntityCommon&)> apply;
            if (k.keyword == "Layer") {
                const Layer* l = findLayer(ctx.doc(), v.text);
                if (!l) {
                    ctx.message("There is no layer named \"" + v.text + "\".");
                    continue;
                }
                const LayerId id = l->id;
                apply = [id](EntityCommon& c) { c.layer = id; };
            } else if (k.keyword == "Color") {
                auto c = parseColor(v.text);
                if (!c) {
                    ctx.message("\"" + v.text + "\" is not a colour.");
                    continue;
                }
                const Color col = *c;
                apply = [col](EntityCommon& x) { x.color = col; };
            } else if (k.keyword == "Linetype") {
                const std::string u = cmd::toUpperAscii(v.text);
                LinetypeId id = kLinetypeByLayer;
                if (u == "BYBLOCK") {
                    id = kLinetypeByBlock;
                } else if (u != "BYLAYER") {
                    const Linetype* lt = ctx.doc().linetypeByName(v.text);
                    if (!lt) {
                        ctx.message("There is no linetype named \"" + v.text + "\".");
                        continue;
                    }
                    id = lt->id;
                }
                apply = [id](EntityCommon& x) { x.linetype = id; };
            } else if (k.keyword == "Lineweight") {
                auto w = parseWeight(v.text);
                if (!w) {
                    ctx.message("\"" + v.text + "\" is not a lineweight.");
                    continue;
                }
                const LineWeight lw = *w;
                apply = [lw](EntityCommon& x) { x.lineweight = lw; };
            } else {
                double scale = 0;
                try {
                    std::size_t used = 0;
                    scale = std::stod(v.text, &used);
                    if (used != v.text.size())
                        scale = 0;
                } catch (...) {
                }
                if (!(scale > 0)) {
                    ctx.message("The linetype scale must be a positive number.");
                    continue;
                }
                apply = [scale](EntityCommon& x) { x.linetypeScale = static_cast<float>(scale); };
            }
            ctx.checkpoint();
            int n = 0;
            for (EntityId id : sel.ids)
                if (ctx.tx().modify(id, [&](Entity& e) { apply(e.common); }))
                    ++n;
            ctx.message(std::to_string(n) + " object(s) changed.");
            co_return;
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeChProp() {
    return std::make_unique<ChProp>();
}

}  // namespace tbcad::select::detail
