// The Viewports dialog (VPORTS): pick a tiled layout for model space with a picture of it, and
// save, restore or delete named configurations. It runs -VPORTS, so it shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/document.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::sysvars {

namespace {

struct Layout {
    const char* label;
    const char* code;
    std::vector<std::vector<float>> rects;  // x0 y0 x1 y1 as fractions, y down
    std::vector<const char*> answers;       // what -VPORTS asks, after its name
};

const std::vector<Layout>& layouts() {
    static const std::vector<Layout> all = {
        {"Single", "1", {{0, 0, 1, 1}}, {"SIngle"}},
        {"Two: Vertical", "2V", {{0, 0, .5f, 1}, {.5f, 0, 1, 1}}, {"2", "Vertical"}},
        {"Two: Horizontal", "2H", {{0, 0, 1, .5f}, {0, .5f, 1, 1}}, {"2", "Horizontal"}},
        {"Three: Right",
         "3R",
         {{0, 0, .5f, .5f}, {0, .5f, .5f, 1}, {.5f, 0, 1, 1}},
         {"3", "Right"}},
        {"Three: Left", "3L", {{0, 0, .5f, 1}, {.5f, 0, 1, .5f}, {.5f, .5f, 1, 1}}, {"3", "Left"}},
        {"Three: Above",
         "3A",
         {{0, 0, 1, .5f}, {0, .5f, .5f, 1}, {.5f, .5f, 1, 1}},
         {"3", "Above"}},
        {"Three: Below",
         "3B",
         {{0, 0, .5f, .5f}, {.5f, 0, 1, .5f}, {0, .5f, 1, 1}},
         {"3", "Below"}},
        {"Three: Vertical",
         "3V",
         {{0, 0, 1 / 3.f, 1}, {1 / 3.f, 0, 2 / 3.f, 1}, {2 / 3.f, 0, 1, 1}},
         {"3", "Vertical"}},
        {"Three: Horizontal",
         "3H",
         {{0, 0, 1, 1 / 3.f}, {0, 1 / 3.f, 1, 2 / 3.f}, {0, 2 / 3.f, 1, 1}},
         {"3", "Horizontal"}},
        {"Four: Equal",
         "4",
         {{0, 0, .5f, .5f}, {.5f, 0, 1, .5f}, {0, .5f, .5f, 1}, {.5f, .5f, 1, 1}},
         {"4"}},
    };
    return all;
}

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

void picture(const Layout& l) {
    const ImVec2 size(180, 120);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(33, 36, 42, 255));
    for (const auto& r : l.rects)
        dl->AddRect(ImVec2(p.x + r[0] * size.x + 2, p.y + r[1] * size.y + 2),
                    ImVec2(p.x + r[2] * size.x - 2, p.y + r[3] * size.y - 2),
                    IM_COL32(150, 190, 240, 255), 0, 0, 1.5f);
    ImGui::Dummy(size);
}

void drawVports(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static std::size_t chosen = 0;
    static std::string saveName{};
    static std::string selectedNamed{};
    const std::string current = pc.host ? pc.host->setting("tmp.vports.layout").value_or("1") : "1";
    ImGui::BeginDisabled(pc.commandActive);
    if (ImGui::BeginTabBar("vports-tabs")) {
        if (ImGui::BeginTabItem("New Viewports")) {
            ImGui::BeginChild("vports-list", ImVec2(170, 260), ImGuiChildFlags_Borders);
            for (std::size_t i = 0; i < layouts().size(); ++i) {
                const Layout& l = layouts()[i];
                const std::string label =
                    std::string(l.label) + (current == l.code ? "  (current)" : "");
                if (ImGui::Selectable(label.c_str(), i == chosen,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    chosen = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        std::vector<std::string> lines = {"-VPORTS"};
                        for (const char* a : l.answers)
                            lines.emplace_back(a);
                        send(pc, lines);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginGroup();
            picture(layouts()[chosen]);
            if (ImGui::Button("Apply")) {
                std::vector<std::string> lines = {"-VPORTS"};
                for (const char* a : layouts()[chosen].answers)
                    lines.emplace_back(a);
                send(pc, lines);
            }
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##vpsave", "Name to save the current tiles as", &saveName);
            ImGui::BeginDisabled(saveName.empty());
            if (ImGui::Button("Save current")) {
                send(pc, {"-VPORTS", "Save", saveName});
                saveName.clear();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Click a viewport to make it active.");
            ImGui::EndGroup();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Named Viewports")) {
            std::vector<std::string> names;
            constexpr std::string_view kPrefix = "tbcad.vports.";
            for (const auto& [k, v] : pc.doc->header().custom)
                if (k.rfind(kPrefix, 0) == 0)
                    names.push_back(k.substr(kPrefix.size()));
            ImGui::BeginChild("vports-named", ImVec2(170, 200), ImGuiChildFlags_Borders);
            if (names.empty())
                ImGui::TextDisabled("None saved yet.");
            for (const std::string& n : names)
                if (ImGui::Selectable(n.c_str(), n == selectedNamed))
                    selectedNamed = n;
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginGroup();
            bool have = false;
            for (const std::string& n : names)
                have = have || n == selectedNamed;
            ImGui::BeginDisabled(!have);
            const std::string sel = selectedNamed;
            if (ImGui::Button("Restore"))
                send(pc, {"-VPORTS", "Restore", sel});
            if (ImGui::Button("Delete"))
                send(pc, {"-VPORTS", "Delete", sel});
            ImGui::EndDisabled();
            ImGui::EndGroup();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
}

}  // namespace

void registerVportsPanel(ui::UiRegistry& ui) {
    ui.addPanel("vports", "Viewports", drawVports, false);
}

}  // namespace tbcad::sysvars
