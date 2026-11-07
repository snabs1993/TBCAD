// PAPERCHECK: how text and dimension text will come out on paper (too small, left at an old
// scale, odd sizes), with the Paper Check panel. PAPERFIT sets them to standard paper heights in
// one undo step.
#include "paper_check.h"

#include <string>

#include "tbcad/cmd/command.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

PaperCheckState& paperCheckState() {
    static PaperCheckState s;
    return s;
}

std::vector<layout::PaperCheckItem> checkScope(const Document& doc,
                                               const std::vector<EntityId>& selected) {
    return layout::paperCheck(doc, selected);
}

std::string paperCheckSummary(const std::vector<layout::PaperCheckItem>& items) {
    using layout::PaperIssue;
    std::size_t n[5] = {};
    for (const auto& it : items)
        ++n[static_cast<int>(it.issue)];
    std::string s = std::to_string(items.size()) + " checked: " + std::to_string(n[0]) + " OK";
    const char* names[5] = {"", "made for another scale", "too small", "too big",
                            "not a standard height"};
    for (int i = 1; i < 5; ++i)
        if (n[i] > 0)
            s += ", " + std::to_string(n[i]) + " " + names[i];
    return s;
}

namespace {

using cmd::KeywordOptions;
using layout::PaperIssue;

class PaperCheckCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAPERCHECK"; }
    std::vector<std::string> defaultAliases() const override { return {"PCHECK"}; }
    std::string_view description() const override {
        return "Check how big text and dimensions print on paper";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        PaperCheckState& st = paperCheckState();
        st.scope = ctx.host().selection();
        st.selectionOnly = !st.scope.empty();
        const auto items =
            checkScope(ctx.doc(), st.selectionOnly ? st.scope : std::vector<EntityId>{});
        if (items.empty())
            ctx.message("No text or dimensions to check.");
        else
            ctx.message("Paper check (" +
                        std::string(st.selectionOnly ? "selected objects" : "whole drawing") +
                        "): " + paperCheckSummary(items) + ".");
        ctx.host().openPanel("papercheck");
        co_return;
    }
};

class PaperFitCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAPERFIT"; }
    std::string_view description() const override {
        return "Set text and dimensions to standard heights on paper";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        KeywordOptions ko;
        ko.keywords = {"All", "Scale", "Small", "Big", "Odd"};
        ko.defaultKeyword = "All";
        auto r = co_await ctx.getKeyword("Fit to paper [All/Scale/Small/Big/Odd] <All>", ko);
        if (!r.ok())
            co_return;
        const std::vector<EntityId> sel = ctx.host().selection();
        auto items = checkScope(ctx.doc(), sel);
        std::vector<layout::PaperCheckItem> pick;
        for (const auto& it : items) {
            const bool want = r.keyword == "All"     ? it.issue != PaperIssue::Ok
                              : r.keyword == "Scale" ? it.issue == PaperIssue::OtherScale
                              : r.keyword == "Small" ? it.issue == PaperIssue::TooSmall
                              : r.keyword == "Big"   ? it.issue == PaperIssue::TooBig
                                                     : it.issue == PaperIssue::OffStandard;
            if (want)
                pick.push_back(it);
        }
        if (pick.empty()) {
            ctx.message("Nothing to fit.");
            co_return;
        }
        auto done = layout::fitToPaper(ctx.tx(), pick);
        if (!done) {
            ctx.message(done.error().message);
            co_return;
        }
        ctx.message("Fitted " + std::to_string(done.value()) +
                    " objects to standard paper heights.");
        if (!sel.empty())
            ctx.host().setSelection({});
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePaperCheckCommand() {
    return std::make_unique<PaperCheckCommand>();
}

std::unique_ptr<cmd::Command> makePaperFitCommand() {
    return std::make_unique<PaperFitCommand>();
}

}  // namespace tbcad::plot
