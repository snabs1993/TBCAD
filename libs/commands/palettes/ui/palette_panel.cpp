#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "../src/commands.h"
#include "tbcad/palettes/palettes.h"

namespace tbcad::palettes {

namespace {

namespace fs = std::filesystem;

struct State {
    std::unique_ptr<PaletteStore> store;
    fs::path file;
    fs::file_time_type stamp{};
    int frames = 0;
    std::string message;
    // Add-tool form.
    Tool draft;
    int kind = 0;
    std::string draftPalette;
    bool openAdd = false;
    // Palette rename / new.
    std::string nameInput;
    bool openNew = false;
    bool openRename = false;
    std::string renameTarget;
};

State& state() {
    static State s;
    return s;
}

fs::file_time_type stampOf(const fs::path& f) {
    std::error_code ec;
    auto t = fs::last_write_time(f, ec);
    return ec ? fs::file_time_type{} : t;
}

void ensureStore(ui::PanelContext& pc, State& st) {
    const fs::path folder = pc.host ? pc.host->settingsFolder() : fs::path{};
    const fs::path file = folder.empty() ? fs::path{} : folder / "palettes.json";
    // Other windows of the program (the content browser) add tools; notice the file changing.
    const bool check = (++st.frames % 30) == 0;
    if (!st.store || file != st.file || (check && stampOf(file) != st.stamp)) {
        st.file = file;
        st.store = std::make_unique<PaletteStore>(file);
        st.store->load();
        st.stamp = stampOf(file);
    }
}

void persist(State& st) {
    st.message = st.store->save();
    st.stamp = stampOf(st.file);
}

void runTool(ui::PanelContext& pc, const Tool& tool) {
    if (!pc.runCommand || !pc.doc)
        return;
    for (const std::string& line : toolLines(tool, *pc.doc))
        pc.runCommand(line);
}

void drawAddPopup(ui::PanelContext& pc, State& st) {
    if (st.openAdd) {
        ImGui::OpenPopup("Add tool");
        st.openAdd = false;
    }
    if (!ImGui::BeginPopupModal("Add tool", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    Tool& t = st.draft;
    ImGui::RadioButton("Block", &st.kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Hatch", &st.kind, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Command", &st.kind, 2);
    t.kind = st.kind == 0 ? ToolKind::Block : st.kind == 1 ? ToolKind::Hatch : ToolKind::Command;
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Name", &t.name);
    if (t.kind == ToolKind::Block) {
        if (ImGui::BeginCombo("Block", t.block.empty() ? "(choose)" : t.block.c_str())) {
            for (const auto& [id, b] : pc.doc->blocks())
                if (!b.anonymous && b.xrefPath.empty() && ImGui::Selectable(b.name.c_str()))
                    t.block = b.name;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("From file (optional)", &t.file);
        ImGui::InputDouble("Scale", &t.scale);
        ImGui::InputDouble("Rotation (deg)", &t.angle);
    } else if (t.kind == ToolKind::Hatch) {
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("Pattern", &t.pattern);
        ImGui::InputDouble("Scale", &t.scale);
        ImGui::InputDouble("Angle (deg)", &t.angle);
    } else {
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Command", &t.command);
        ImGui::TextDisabled("Several lines: separate them with |");
    }
    if (ImGui::BeginCombo("Layer", t.layer.empty() ? "(keep current)" : t.layer.c_str())) {
        if (ImGui::Selectable("(keep current)"))
            t.layer.clear();
        for (const auto& [id, l] : pc.doc->layers())
            if (!l.parent && ImGui::Selectable(l.name.c_str()))
                t.layer = l.name;
        ImGui::EndCombo();
    }
    if (!st.message.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", st.message.c_str());
    if (ImGui::Button("Add", ImVec2(100, 0))) {
        st.message = st.store->addTool(st.draftPalette, t);
        if (st.message.empty()) {
            persist(st);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void drawNamePopup(State& st, const char* title, bool& open, bool rename) {
    if (open) {
        ImGui::OpenPopup(title);
        open = false;
    }
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::SetNextItemWidth(260);
    const bool enter =
        ImGui::InputText("##paletteName", &st.nameInput, ImGuiInputTextFlags_EnterReturnsTrue);
    if (!st.message.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", st.message.c_str());
    if (ImGui::Button("OK", ImVec2(100, 0)) || enter) {
        st.message = rename ? st.store->renamePalette(st.renameTarget, st.nameInput)
                            : st.store->addPalette(st.nameInput);
        if (st.message.empty()) {
            persist(st);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void drawPanel(ui::PanelContext& pc) {
    if (!pc.doc)
        return;
    State& st = state();
    ensureStore(pc, st);
    if (!st.store->warning().empty())
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", st.store->warning().c_str());

    if (ImGui::BeginTabBar("##palettes", ImGuiTabBarFlags_AutoSelectNewTabs)) {
        // Copy the names: the store changes while a context menu is handled.
        std::vector<std::string> names;
        for (const Palette& p : st.store->palettes())
            names.push_back(p.name);
        for (const std::string& name : names) {
            const bool open = ImGui::BeginTabItem(name.c_str());
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename...")) {
                    st.openRename = true;
                    st.renameTarget = name;
                    st.nameInput = name;
                    st.message.clear();
                }
                if (ImGui::MenuItem("Delete palette")) {
                    st.store->removePalette(name);
                    persist(st);
                }
                ImGui::EndPopup();
            }
            if (!open)
                continue;
            Palette* p = st.store->find(name);
            if (!p) {
                ImGui::EndTabItem();
                continue;
            }
            std::string removeTool, moveTool;
            int moveBy = 0;
            const float width = std::max(110.0f, ImGui::GetContentRegionAvail().x);
            float x = 0;
            for (const Tool& t : p->tools) {
                const float w = 120.0f;
                if (x + w > width && x > 0)
                    x = 0;
                else if (x > 0)
                    ImGui::SameLine();
                x += w + ImGui::GetStyle().ItemSpacing.x;
                if (ImGui::Button((t.name + "##" + name + t.name).c_str(), ImVec2(w, 34)))
                    runTool(pc, t);
                if (ImGui::IsItemHovered()) {
                    const auto lines = toolLines(t, *pc.doc);
                    std::string tip;
                    for (const std::string& l : lines)
                        tip += (tip.empty() ? "" : "  >  ") + (l.empty() ? "(Enter)" : l);
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Move left")) {
                        moveTool = t.name;
                        moveBy = -1;
                    }
                    if (ImGui::MenuItem("Move right")) {
                        moveTool = t.name;
                        moveBy = 1;
                    }
                    if (ImGui::MenuItem("Remove"))
                        removeTool = t.name;
                    ImGui::EndPopup();
                }
            }
            if (!removeTool.empty()) {
                st.store->removeTool(name, removeTool);
                persist(st);
            } else if (!moveTool.empty()) {
                st.store->moveTool(name, moveTool, moveBy);
                persist(st);
            }
            ImGui::Spacing();
            if (ImGui::SmallButton("+ Add tool")) {
                st.draft = Tool{};
                st.draft.scale = 1;
                st.kind = 0;
                st.draftPalette = name;
                st.openAdd = true;
                st.message.clear();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            st.openNew = true;
            st.nameInput.clear();
            st.message.clear();
        }
        ImGui::EndTabBar();
    }
    drawAddPopup(pc, st);
    drawNamePopup(st, "New palette", st.openNew, false);
    drawNamePopup(st, "Rename palette", st.openRename, true);
}

}  // namespace

void registerPalettePanel(ui::UiRegistry& ui) {
    ui.addPanel("palettes", "Tool Palettes", drawPanel, false);
}

}  // namespace tbcad::palettes
