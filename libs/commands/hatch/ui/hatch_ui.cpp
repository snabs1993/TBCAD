// Hatch panel, property section and menu entries (Dear ImGui). Built only in application builds.
#include <imgui.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../src/hatch_params.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::hatchcmd {

namespace {

using namespace tbcad::hatch;

// A pattern drawn once into a 30 x 30 mm square, cached by name.
struct Thumb {
    std::vector<std::pair<Vec2, Vec2>> segments{};
    std::vector<Vec2> dots{};
    bool solid = false;
};

constexpr double kThumbMm = 30;

const Thumb& thumbFor(const Pattern& pattern) {
    static std::map<std::string, Thumb> cache;
    auto it = cache.find(pattern.name);
    if (it != cache.end())
        return it->second;
    HatchData h;
    HatchLoop loop;
    Vec2 c[4] = {{0, 0}, {kThumbMm, 0}, {kThumbMm, kThumbMm}, {0, kThumbMm}};
    for (int i = 0; i < 4; ++i)
        loop.edges.push_back(HatchLineEdge{c[i], c[(i + 1) % 4]});
    h.loops.push_back(std::move(loop));
    h.solid = false;
    FillResult r = generateFill(h, pattern);
    Thumb t;
    t.solid = r.mode != FillResult::Mode::Lines;
    t.segments = std::move(r.segments);
    t.dots = std::move(r.dots);
    return cache.emplace(pattern.name, std::move(t)).first->second;
}

void drawThumb(ImDrawList* dl, ImVec2 min, float size, const Pattern& pattern) {
    const Thumb& t = thumbFor(pattern);
    ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 max{min.x + size, min.y + size};
    dl->AddRectFilled(min, max, bg);
    float k = size / static_cast<float>(kThumbMm);
    auto map = [&](Vec2 p) {
        return ImVec2(min.x + static_cast<float>(p.x) * k, max.y - static_cast<float>(p.y) * k);
    };
    if (t.solid) {
        dl->AddRectFilled(min, max, fg);
    } else {
        for (const auto& [a, b] : t.segments)
            dl->AddLine(map(a), map(b), fg, 1.0f);
        for (const Vec2& d : t.dots)
            dl->AddCircleFilled(map(d), 1.0f, fg);
    }
    dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
}

std::string selectionCommand(const Params& p, bool includeBase) {
    std::string line = "HATCHEDIT P " + p.pattern + " S " + number(p.scale) + " A " +
                       number(p.angle * 180.0 / kPi);
    if (includeBase)
        line += " B " + number(p.base.x) + "," + number(p.base.y);
    return line + " D";
}

bool hasHatchSelected(const ui::PanelContext& pc) {
    if (!pc.doc || !pc.selection)
        return false;
    // The panel asks on every frame; a big selection is only scanned again when it changed.
    static const Document* cachedDoc = nullptr;
    static std::uint64_t cachedRevision = 0;
    static std::size_t cachedSize = 0;
    static EntityId cachedFront{}, cachedBack{};
    static bool cachedResult = false;
    const auto& sel = *pc.selection;
    const EntityId front = sel.empty() ? EntityId{} : sel.front();
    const EntityId back = sel.empty() ? EntityId{} : sel.back();
    if (cachedDoc == pc.doc && cachedRevision == pc.doc->revision() && cachedSize == sel.size() &&
        cachedFront == front && cachedBack == back)
        return cachedResult;
    bool found = false;
    for (EntityId id : sel)
        if (const Entity* e = pc.doc->find(id); e && kind(*e) == EntityKind::Hatch) {
            found = true;
            break;
        }
    cachedDoc = pc.doc;
    cachedRevision = pc.doc->revision();
    cachedSize = sel.size();
    cachedFront = front;
    cachedBack = back;
    cachedResult = found;
    return found;
}

// ---- Panel: pattern list with thumbnails, parameters, pick / apply buttons -------------------

// Label on the left, box filling the rest of the row (matches the Properties panel).
std::string leftLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(70);
    ImGui::SetNextItemWidth(-1);
    return std::string("##") + label;
}

void drawPanel(ui::PanelContext& pc) {
    if (!pc.host)
        return;
    Params p = loadParams(*pc.host);
    bool changed = false;

    ImGui::BeginChild("hatch-patterns", ImVec2(170, 0), ImGuiChildFlags_Borders);
    for (const std::string& name : PatternRegistry::names()) {
        const Pattern* pattern = PatternRegistry::find(name);
        if (!pattern)
            continue;
        ImGui::PushID(name.c_str());
        ImVec2 min = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##pattern", p.pattern == name, 0, ImVec2(0, 44))) {
            p.pattern = name;
            changed = true;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawThumb(dl, ImVec2(min.x + 2, min.y + 2), 40, *pattern);
        dl->AddText(ImVec2(min.x + 50, min.y + 14), ImGui::GetColorU32(ImGuiCol_Text),
                    name.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (const Pattern* pattern = PatternRegistry::find(p.pattern))
        ImGui::TextWrapped("%s", pattern->description.c_str());
    double scale = p.scale;
    if (ImGui::InputDouble(leftLabel("Scale").c_str(), &scale, 0, 0, "%.6g",
                           ImGuiInputTextFlags_AutoSelectAll) &&
        scale > 0) {
        p.scale = scale;
        p.scaleAuto = false;
        changed = true;
    }
    double angle = p.angle * 180.0 / kPi;
    if (ImGui::InputDouble(leftLabel("Angle").c_str(), &angle, 0, 0, "%.6g",
                           ImGuiInputTextFlags_AutoSelectAll)) {
        p.angle = angle * kPi / 180.0;
        changed = true;
    }
    double base[2] = {p.base.x, p.base.y};
    if (ImGui::InputScalarN(leftLabel("Origin").c_str(), ImGuiDataType_Double, base, 2, nullptr,
                            nullptr, "%.6g")) {
        p.base = {base[0], base[1]};
        changed = true;
    }
    if (changed)
        saveParams(*pc.host, p);

    if (ImGui::Button("Pick internal point") && pc.runCommand)
        pc.runCommand("HATCH");
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasHatchSelected(pc));
    if (ImGui::Button("Apply to selected hatches") && pc.runCommand)
        pc.runCommand(selectionCommand(p, false));
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

// ---- Properties panel: Hatch section ---------------------------------------------------------

void drawSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc || !pc.runCommand || pc.multiSelection)
        return;
    const Entity* e = pc.doc->find(id);
    const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
    if (!h)
        return;
    ImGui::SeparatorText("Hatch");

    if (ImGui::BeginCombo("Pattern", h->pattern.empty() ? "SOLID" : h->pattern.c_str())) {
        for (const std::string& name : PatternRegistry::names()) {
            if (ImGui::Selectable(name.c_str(), name == h->pattern))
                pc.runCommand("HATCHEDIT P " + name + " D");
        }
        ImGui::EndCombo();
    }
    // InputDouble does not take EnterReturnsTrue: the edit counts when the field is left.
    constexpr ImGuiInputTextFlags kEnter = ImGuiInputTextFlags_AutoSelectAll;
    double scale = h->patternScale;
    ImGui::InputDouble("Scale", &scale, 0, 0, "%.6g", kEnter);
    if (ImGui::IsItemDeactivatedAfterEdit() && scale > 0 && scale != h->patternScale)
        pc.runCommand("HATCHEDIT S " + number(scale) + " D");
    double angle = h->patternAngle * 180.0 / kPi;
    ImGui::InputDouble("Angle", &angle, 0, 0, "%.6g", kEnter);
    if (ImGui::IsItemDeactivatedAfterEdit())
        pc.runCommand("HATCHEDIT A " + number(angle) + " D");
    double base[2] = {h->patternOrigin.x, h->patternOrigin.y};
    ImGui::InputScalarN("Origin", ImGuiDataType_Double, base, 2, nullptr, nullptr, "%.6g", kEnter);
    if (ImGui::IsItemDeactivatedAfterEdit())
        pc.runCommand("HATCHEDIT B " + number(base[0]) + "," + number(base[1]) + " D");
    ImGui::TextDisabled("%zu loop(s)", h->loops.size());
}

// ---- Contextual tab: Hatch Editor --------------------------------------------------------------

void drawContextTab(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand || !pc.selection || pc.selection->empty())
        return;
    const Entity* e = pc.doc->find(pc.selection->front());
    const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
    if (!h)
        return;
    // The values shown are the first hatch's; an edit goes to every selected hatch.
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##ctxpattern", h->solid ? "SOLID" : h->pattern.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        for (const std::string& name : PatternRegistry::names())
            if (ImGui::Selectable(name.c_str(), name == h->pattern))
                pc.runCommand("HATCHEDIT P " + name + " D");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pattern");
    constexpr ImGuiInputTextFlags kEnter = ImGuiInputTextFlags_AutoSelectAll;
    ImGui::SameLine();
    double scale = h->patternScale;
    ImGui::SetNextItemWidth(70);
    ImGui::InputDouble("Scale##ctx", &scale, 0, 0, "%.4g", kEnter);
    if (ImGui::IsItemDeactivatedAfterEdit() && scale > 0 && scale != h->patternScale)
        pc.runCommand("HATCHEDIT S " + number(scale) + " D");
    ImGui::SameLine();
    double angle = h->patternAngle * 180.0 / kPi;
    ImGui::SetNextItemWidth(60);
    ImGui::InputDouble("Angle##ctx", &angle, 0, 0, "%.4g", kEnter);
    if (ImGui::IsItemDeactivatedAfterEdit())
        pc.runCommand("HATCHEDIT A " + number(angle) + " D");
    for (const auto& [label, command] :
         {std::pair{"Edit...", "HATCHEDIT"}, std::pair{"Send to back", "HATCHTOBACK"},
          std::pair{"Explode", "EXPLODE"}}) {
        ImGui::SameLine();
        if (ImGui::SmallButton(label))
            pc.runCommand(command);
    }
}

}  // namespace

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Hatch", "HATCH", "draw.hatch");
    ui.addMenuItem("Draw/Gradient", "GRADIENT", "draw.gradient");
    ui.addMenuItem("Modify/Edit Hatch", "HATCHEDIT", "modify.hatchedit");
    ui.addPanel("hatch", "Hatch", drawPanel, false);
    ui.addPropertySection(EntityKind::Hatch, drawSection);
    ui.addContextualTab("hatch", "Hatch Editor", {EntityKind::Hatch}, drawContextTab);
}

}  // namespace tbcad::hatchcmd
