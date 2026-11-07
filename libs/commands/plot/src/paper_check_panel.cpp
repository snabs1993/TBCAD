// The Paper Check panel ("papercheck"): text and dimensions grouped by how they print (made for
// another scale, too small, too big, odd heights, OK), a place filter, Show and Fix per group and
// Fit all. While the panel is open the checked objects are outlined on the drawing in the colour
// of their result. Fixes run PAPERFIT, so each is one undo step on the command line.
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "panel.h"
#include "paper_check.h"
#include "tbcad/geom/entity.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

using layout::PaperCheckItem;
using layout::PaperIssue;

struct PanelCache {
    std::uint64_t revision = ~std::uint64_t{0};
    const Document* doc = nullptr;
    bool selectionOnly = false;
    std::size_t scopeSize = 0;
    std::vector<PaperCheckItem> items{};  // every checked object
    std::string place{};                  // "" = every place
    int lastFrame = -10;                  // ImGui frame the panel was last drawn
};

PanelCache& cache() {
    static PanelCache c;
    return c;
}

Rgba issueColor(PaperIssue i) {
    switch (i) {
        case PaperIssue::Ok:
            return {26, 127, 55, 255};
        case PaperIssue::OffStandard:
            return {191, 135, 0, 255};
        default:
            return {207, 34, 46, 255};
    }
}

ImVec4 toImVec(Rgba c) {
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
}

std::string placeName(const PaperCheckItem& it) {
    return layout::describePaperPlace(it.place);
}

void refresh(ui::PanelContext& pc) {
    PanelCache& c = cache();
    const PaperCheckState& st = paperCheckState();
    if (c.doc == pc.doc && c.revision == pc.doc->revision() &&
        c.selectionOnly == st.selectionOnly && c.scopeSize == st.scope.size())
        return;
    c.doc = pc.doc;
    c.revision = pc.doc->revision();
    c.selectionOnly = st.selectionOnly;
    c.scopeSize = st.scope.size();
    c.items = checkScope(*pc.doc, st.selectionOnly ? st.scope : std::vector<EntityId>{});
}

void run(ui::PanelContext& pc, std::initializer_list<std::string> lines) {
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

std::string mm(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (s.back() == '0')
        s.pop_back();
    if (s.back() == '.')
        s.pop_back();
    return s + " mm";
}

void draw(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host || !pc.runCommand)
        return;
    PanelCache& c = cache();
    PaperCheckState& st = paperCheckState();
    c.lastFrame = ImGui::GetFrameCount();
    refresh(pc);

    // Scope: the whole drawing or the objects selected when the check ran.
    ImGui::TextUnformatted(
        st.selectionOnly ? ("Selected objects (" + std::to_string(st.scope.size()) + ")").c_str()
                         : "Whole drawing");
    ImGui::SameLine();
    if (ImGui::SmallButton("Check whole drawing")) {
        st.selectionOnly = false;
        st.scope.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(pc.selection == nullptr || pc.selection->empty());
    if (ImGui::SmallButton("Check selection")) {
        st.scope = *pc.selection;
        st.selectionOnly = true;
    }
    ImGui::EndDisabled();

    // Place filter: one frame, viewport, zone or sheet.
    std::vector<std::string> places;
    for (const auto& it : c.items)
        if (std::find(places.begin(), places.end(), placeName(it)) == places.end())
            places.push_back(placeName(it));
    if (std::find(places.begin(), places.end(), c.place) == places.end())
        c.place.clear();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##place", c.place.empty() ? "Every frame and sheet" : c.place.c_str())) {
        if (ImGui::Selectable("Every frame and sheet", c.place.empty()))
            c.place.clear();
        for (const auto& p : places)
            if (ImGui::Selectable(p.c_str(), c.place == p))
                c.place = p;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Standard heights: 1.8  2.5  3.5  5  7  10 mm");
    ImGui::Checkbox("Colour on drawing", &st.colours);
    ImGui::Separator();

    if (c.items.empty()) {
        ImGui::TextWrapped("No text or dimensions to check.");
        return;
    }

    // Groups in order of how bad they are.
    const PaperIssue order[] = {PaperIssue::OtherScale, PaperIssue::TooSmall, PaperIssue::TooBig,
                                PaperIssue::OffStandard, PaperIssue::Ok};
    const char* keyword[] = {"Scale", "Small", "Big", "Odd", ""};
    std::size_t problems = 0;
    std::vector<EntityId> problemIds;
    for (int g = 0; g < 5; ++g) {
        std::vector<const PaperCheckItem*> group;
        for (const auto& it : c.items)
            if (it.issue == order[g] && (c.place.empty() || placeName(it) == c.place))
                group.push_back(&it);
        if (group.empty())
            continue;
        ImGui::PushID(g);
        const ImVec4 col = toImVec(issueColor(order[g]));
        ImGui::TextColored(col, "|");
        ImGui::SameLine();
        ImGui::Text("%s (%d)", layout::paperIssueLabel(order[g]), static_cast<int>(group.size()));
        const PaperCheckItem& first = *group.front();
        std::string line;
        if (order[g] == PaperIssue::Ok)
            line = "Reads well on paper";
        else if (order[g] == PaperIssue::OtherScale)
            line = mm(first.mm) +
                   ", looks made for 1:" + std::to_string(static_cast<int>(first.madeForN)) +
                   "  ->  " + mm(first.suggestMm);
        else
            line = mm(first.mm) + "  ->  " + mm(first.suggestMm);
        if (group.size() > 1)
            line += "  (first of " + std::to_string(group.size()) + ")";
        ImGui::TextDisabled("%s", line.c_str());
        std::vector<EntityId> ids;
        for (const auto* it : group)
            ids.push_back(it->id);
        if (ImGui::SmallButton("Show")) {
            pc.host->setSelection(ids);
            run(pc, {"ZOOM", "Object"});
        }
        if (order[g] != PaperIssue::Ok) {
            problems += group.size();
            problemIds.insert(problemIds.end(), ids.begin(), ids.end());
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix")) {
                pc.host->setSelection(ids);
                run(pc, {"PAPERFIT", keyword[g]});
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Set these to the suggested height (one undo)");
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(problems == 0);
    if (ImGui::Button("Fit all to paper", ImVec2(-1, 0))) {
        pc.host->setSelection(problemIds);
        run(pc, {"PAPERFIT", "All"});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every problem above to its suggested height, in one undo step");
}

}  // namespace

void registerPaperCheckPanel(ui::UiRegistry& registry) {
    registry.addPanel("papercheck", "Paper Check", draw, false);
    registry.addMenuItem("Dimension/Check Paper Sizes...", "PAPERCHECK", "dimension.paper_check");
    // While the panel is open: each checked object on the shown space outlined in its colour.
    registry.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        PanelCache& c = cache();
        if (!ctx.doc || !ctx.host || c.doc != ctx.doc || !paperCheckState().colours ||
            ImGui::GetFrameCount() - c.lastFrame > 2)
            return;
        const BlockId owner = ctx.host->drawingOwner();
        render::OverlayStyle s;
        s.widthPx = 2.0;
        for (const auto& it : c.items) {
            if (!c.place.empty() && placeName(it) != c.place)
                continue;
            const Entity* e = ctx.doc->find(it.id);
            if (!e || ctx.doc->ownerBlock(it.id) != owner)
                continue;
            const BBox2 b = geom::bounds(*e, *ctx.doc);
            if (b.empty())
                continue;
            s.color = issueColor(it.issue);
            const Vec2 a = b.min, z = b.max;
            const std::vector<Vec2> pts{a, {z.x, a.y}, z, {a.x, z.y}};
            overlay.polyline(render::Space::World, pts, true, s);
        }
    });
}

}  // namespace tbcad::plot
