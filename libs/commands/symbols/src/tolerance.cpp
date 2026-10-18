#include "tbcad/cmd/cmd.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/symbolscmd/symbolscmd.h"
#include "tbcad/text/annotation_size.h"

namespace tbcad::symbolscmd {

namespace {

class ToleranceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TOLERANCE"; }
    std::vector<std::string> defaultAliases() const override { return {"TOL"}; }
    std::string_view description() const override {
        return "Draw a geometric tolerance (feature control frame)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        gdt::Spec spec;
        cmd::KeywordOptions ko;
        for (int i = 0; i < gdt::kSymbolCount; ++i)
            ko.keywords.emplace_back(gdt::symbolName(static_cast<gdt::Symbol>(i)));
        ko.defaultKeyword = "Position";
        auto k =
            co_await ctx.getKeyword("Geometric characteristic [" + joined(ko.keywords) + "]", ko);
        if (!k.ok())
            co_return;
        if (auto sym = gdt::symbolByName(k.keyword))
            spec.symbol = *sym;

        cmd::StringOptions vo;
        auto v = co_await ctx.getString("Tolerance value, such as 0.05", vo);
        if (!v.ok())
            co_return;
        spec.value = cmd::trim(v.text);

        cmd::KeywordOptions dz;
        dz.keywords = {"Yes", "No"};
        dz.defaultKeyword = "No";
        auto z = co_await ctx.getKeyword("Diameter tolerance zone [Yes/No] <No>", dz);
        if (!z.ok())
            co_return;
        spec.diameterZone = z.keyword == "Yes";

        cmd::KeywordOptions mc;
        mc.keywords = {"None", "Mmc", "Lmc"};
        mc.defaultKeyword = "None";
        auto m = co_await ctx.getKeyword("Material condition [None/Mmc/Lmc] <None>", mc);
        if (!m.ok())
            co_return;
        spec.material = m.keyword == "Mmc" ? 'M' : m.keyword == "Lmc" ? 'L' : 0;

        static const char* kOrder[] = {"Primary", "Secondary", "Tertiary"};
        for (const char* which : kOrder) {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto d = co_await ctx.getString(std::string(which) + " datum letter <none>", so);
            if (!d.ok() || cmd::trim(d.text).empty())
                break;
            spec.datums.push_back(cmd::trim(d.text));
        }

        auto p = co_await ctx.getPoint("Specify the frame position (left edge, middle)");
        if (!p.ok())
            co_return;

        // One block per frame, inserted at the annotation scale like the symbols.
        std::string blockName;
        for (int i = 1;; ++i) {
            blockName = "TB_TOL_" + std::to_string(i);
            if (!ctx.doc().blockByName(blockName))
                break;
        }
        BlockDef def;
        def.name = blockName;
        auto id = ctx.tx().addBlock(std::move(def));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        for (Entity& e : gdt::frameEntities(spec)) {
            e.common.layer = kLayerZero;
            auto added = ctx.tx().add(std::move(e), id.value());
            if (!added) {
                ctx.message(added.error().message);
                co_return;
            }
        }
        auto remembered = gdt::addFrame(ctx.tx(), blockName, spec);
        if (!remembered) {
            ctx.message(remembered.error().message);
            co_return;
        }
        const double n = text::suggestedAnnotationScale(
            ctx.doc(), ctx.host().view().scale, static_cast<bool>(ctx.host().drawingOwner()),
            gdt::kTextHeight);
        Entity e;
        e.data = Indirect<InsertData>(InsertData{id.value(), p.point, {n, n}, 0});
        applyCurrentProperties(ctx.doc().header(), e);
        e.common.annotative = true;
        e.common.annoScales = ScaleList(std::vector<double>{n});
        auto ins = ctx.tx().add(std::move(e));
        if (!ins) {
            ctx.message(ins.error().message);
            co_return;
        }
        ctx.setUndoLabel("TOLERANCE");
    }

private:
    static std::string joined(const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& k : v)
            s += (s.empty() ? "" : "/") + k;
        return s;
    }
};

}  // namespace

void registerToleranceCommand(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ToleranceCommand>());
}

}  // namespace tbcad::symbolscmd
