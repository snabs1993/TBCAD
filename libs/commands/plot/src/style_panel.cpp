#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <cmath>
#include <string>

#include "panel.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

using plotstyle::PlotStyleEntry;
using plotstyle::PlotStyleTable;

constexpr std::array<double, 16> kWeights = {0.0,  0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25,
                                             0.30, 0.35, 0.40, 0.50, 0.70, 1.00, 1.40, 2.00};

struct State {
    PlotStyleTable table{};
    std::string loadedFrom{};  // the setting value the table was loaded for
    std::string saveName{};
    std::string message{};
    bool dirty = false;
};

State& state() {
    static State s;
    return s;
}

void load(ui::PanelContext& pc, State& st) {
    const std::string name = plotStyleName(*pc.host);
    auto t = resolveStyleTable(*pc.host, name);
    st.table = (t && t.value()) ? *t.value() : PlotStyleTable{};
    st.loadedFrom = name;
    st.saveName = st.table.name;
    st.dirty = false;
    st.message = t ? std::string() : t.error().message;
}

void draw(ui::PanelContext& pc) {
    State& st = state();
    if (!pc.host)
        return;
    if (st.loadedFrom != plotStyleName(*pc.host) && !st.dirty)
        load(pc, st);

    if (ImGui::BeginCombo("Table", st.loadedFrom.c_str())) {
        for (const auto& n : listStyleTables(*pc.host))
            if (ImGui::Selectable(n.c_str(), n == st.loadedFrom)) {
                setPlotStyleName(*pc.host, n);
                load(pc, st);
            }
        if (ImGui::Selectable("None", false)) {
            setPlotStyleName(*pc.host, "none");
            load(pc, st);
        }
        ImGui::EndCombo();
    }
    ImGui::InputText("Save as", &st.saveName);
    ImGui::SameLine();
    if (ImGui::Button("Save") && !st.saveName.empty()) {
        auto folder = plotStyleFolder(*pc.host);
        if (folder.empty()) {
            st.message = "There is no folder for plot style tables on this system.";
        } else {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            st.table.name = st.saveName;
            auto r = plotstyle::saveFile(st.table, folder / (st.saveName + ".tbstyle"));
            st.message = r ? "Saved " + st.saveName + ".tbstyle" : r.error().message;
            if (r) {
                setPlotStyleName(*pc.host, st.saveName + ".tbstyle");
                st.loadedFrom = st.saveName + ".tbstyle";
                st.dirty = false;
            }
        }
    }
    if (!st.message.empty())
        ImGui::TextWrapped("%s", st.message.c_str());
    ImGui::TextDisabled("Built-in tables cannot be changed: edit, then Save under a new name.");
    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("styles", 6, flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Colour");
    ImGui::TableSetupColumn("Plot colour");
    ImGui::TableSetupColumn("Lineweight");
    ImGui::TableSetupColumn("Screen %");
    ImGui::TableSetupColumn("Grey");
    ImGui::TableSetupColumn("Label");
    ImGui::TableHeadersRow();
    for (int i = 1; i < 256; ++i) {
        PlotStyleEntry& e = st.table.entries[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const Rgba pc0 = paletteColor(static_cast<std::uint8_t>(i));
        ImGui::ColorButton("##sw", ImVec4(pc0.r / 255.f, pc0.g / 255.f, pc0.b / 255.f, 1),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        ImGui::Text("%d", i);

        ImGui::TableSetColumnIndex(1);
        bool useObject = !e.color.has_value();
        if (ImGui::Checkbox("object", &useObject)) {
            e.color = useObject ? std::nullopt : std::optional<Rgba>(Rgba{0, 0, 0, 255});
            st.dirty = true;
        }
        if (e.color) {
            ImGui::SameLine();
            float c[3] = {e.color->r / 255.f, e.color->g / 255.f, e.color->b / 255.f};
            if (ImGui::ColorEdit3("##c", c, ImGuiColorEditFlags_NoInputs)) {
                e.color = Rgba{static_cast<std::uint8_t>(std::lround(c[0] * 255)),
                               static_cast<std::uint8_t>(std::lround(c[1] * 255)),
                               static_cast<std::uint8_t>(std::lround(c[2] * 255)), 255};
                st.dirty = true;
            }
        }

        ImGui::TableSetColumnIndex(2);
        char label[24];
        if (e.lineweightMm)
            std::snprintf(label, sizeof label, "%.2f mm", *e.lineweightMm);
        else
            std::snprintf(label, sizeof label, "object");
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##w", label)) {
            if (ImGui::Selectable("object", !e.lineweightMm)) {
                e.lineweightMm.reset();
                st.dirty = true;
            }
            for (double w : kWeights) {
                char wl[24];
                std::snprintf(wl, sizeof wl, "%.2f mm", w);
                if (ImGui::Selectable(wl, e.lineweightMm && std::abs(*e.lineweightMm - w) < 1e-9)) {
                    e.lineweightMm = w;
                    st.dirty = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::SetNextItemWidth(90);
        if (ImGui::SliderInt("##s", &e.screen, 0, 100))
            st.dirty = true;

        ImGui::TableSetColumnIndex(4);
        if (ImGui::Checkbox("##g", &e.grayscale))
            st.dirty = true;

        ImGui::TableSetColumnIndex(5);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputText("##n", &e.name))
            st.dirty = true;
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

void registerStylePanel(ui::UiRegistry& registry) {
    registry.addPanel("plotstyle", "Plot Styles", draw, false);
}

}  // namespace tbcad::plot
