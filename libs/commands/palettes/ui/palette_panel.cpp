#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "../src/commands.h"
#include "tbcad/palettes/palettes.h"
#include "tbcad/ui/drag_payload.h"

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
    std::string editing;  // the tool whose Properties are open; empty = adding a new tool
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

// The Add tool form, also the Properties of an existing tool (st.editing).
void drawAddPopup(ui::PanelContext& pc, State& st) {
    if (st.openAdd) {
        ImGui::OpenPopup("Add tool");
        st.openAdd = false;
    }
    if (!ImGui::BeginPopupModal("Add tool", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (!st.editing.empty())
        ImGui::TextDisabled("Properties of %s", st.editing.c_str());
    Tool& t = st.draft;
    ImGui::RadioButton("Block", &st.kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Hatch", &st.kind, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Command", &st.kind, 2);
    t.kind = st.kind == 0 ? ToolKind::Block : st.kind == 1 ? ToolKind::Hatch : ToolKind::Command;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
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
    if (ImGui::Button(st.editing.empty() ? "Add" : "OK", ImVec2(100, 0))) {
        st.message = st.editing.empty() ? st.store->addTool(st.draftPalette, t)
                                        : st.store->replaceTool(st.draftPalette, st.editing, t);
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
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
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

// Blocks dragged from the content browser or the Blocks palette land here and become tools of the
// palette. A block of the open drawing remembers the drawing's file, so the tool works in other
// drawings too once that drawing is saved.
void drawDropZone(ui::PanelContext& pc, State& st, const std::string& palette) {
    ImGui::Spacing();
    ImGui::Button("Drop blocks here to add them", ImVec2(-1, 28));
    if (!ImGui::BeginDragDropTarget())
        return;
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(ui::kContentPayload)) {
        const ui::ContentDrag drag = ui::decodeContent(static_cast<const char*>(pl->Data));
        if (drag.kind == "Block") {
            for (const std::string& n : drag.names) {
                Tool t;
                t.name = n;
                t.kind = ToolKind::Block;
                t.block = n;
                t.file = drag.file;
                if (t.file.empty() && pc.host)
                    t.file = pc.host->documentFile();
                (void)st.store->addTool(palette, std::move(t));  // a name already there is skipped
            }
            persist(st);
        }
    }
    ImGui::EndDragDropTarget();
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
            std::string removeTool, moveTool, moveTo;
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
                if (ImGui::BeginDragDropSource()) {
                    const std::string data = ui::encodeTool(name, t.name, toolLines(t, *pc.doc));
                    ImGui::SetDragDropPayload(ui::kCommandsPayload, data.c_str(), data.size() + 1);
                    ImGui::Text("%s", t.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {  // drop another tool of this palette here
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload(ui::kCommandsPayload)) {
                        const std::string data(static_cast<const char*>(pl->Data));
                        const std::string origin = data.substr(0, data.find('\n'));
                        const size_t tab = origin.find('\t');
                        if (tab != std::string::npos && origin.substr(0, tab) == name) {
                            moveTool = origin.substr(tab + 1);
                            moveTo = t.name;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0)) {
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
                    if (ImGui::MenuItem("Properties...")) {
                        st.draft = t;
                        st.kind = t.kind == ToolKind::Block ? 0 : t.kind == ToolKind::Hatch ? 1 : 2;
                        st.draftPalette = name;
                        st.editing = t.name;
                        st.openAdd = true;
                        st.message.clear();
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
                if (!moveTo.empty()) {  // dragged: land where the target tool is
                    int from = -1, to = -1, i = 0;
                    for (const Tool& t : p->tools) {
                        from = t.name == moveTool ? i : from;
                        to = t.name == moveTo ? i : to;
                        ++i;
                    }
                    moveBy = (from >= 0 && to >= 0) ? to - from : 0;
                }
                if (moveBy != 0) {
                    st.store->moveTool(name, moveTool, moveBy);
                    persist(st);
                }
            }
            drawDropZone(pc, st, name);
            ImGui::Spacing();
            if (ImGui::SmallButton("+ Add tool")) {
                st.draft = Tool{};
                st.draft.scale = 1;
                st.kind = 0;
                st.draftPalette = name;
                st.editing.clear();
                st.openAdd = true;
                st.message.clear();
            }
            // Tools that draw more objects like the selected ones (block, hatch, or the command
            // that makes them), on their layer.
            const bool haveSelection = pc.selection && !pc.selection->empty();
            ImGui::SameLine();
            ImGui::BeginDisabled(!haveSelection);
            if (ImGui::SmallButton("+ Selected objects") && haveSelection) {
                for (Tool& t : toolsFromObjects(*pc.doc, *pc.selection,
                                                pc.host ? pc.host->documentFile() : std::string()))
                    (void)st.store->addTool(name, std::move(t));  // a name already there is skipped
                persist(st);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Add tools that make objects like the selected ones");
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
