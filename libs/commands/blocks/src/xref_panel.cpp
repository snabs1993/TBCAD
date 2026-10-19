#include <filesystem>
#include <string>
#include <vector>

#include "commands.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "tbcad/xref/xref.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

struct XrefPanelState {
    BlockId selected{};
    std::string relink{};
    BlockId relinkFor{};
};

XrefPanelState& state() {
    static XrefPanelState s;
    return s;
}

const char* stateLabel(xref::State s) {
    switch (s) {
        case xref::State::UpToDate:
            return "Loaded";
        case xref::State::Changed:
            return "Needs reload";
        case xref::State::Missing:
            return "Not found";
        case xref::State::Unloaded:
            return "Unloaded";
        case xref::State::NotXref:
            break;
    }
    return "";
}

const char* typeLabel(const BlockDef& d) {
    if (underlay::isImagePath(d.xrefPath))
        return "Image";
    if (underlay::isPdfPath(d.xrefPath))
        return "PDF";
    return d.xrefOverlay ? "Overlay" : "Attach";
}

// Runs a command that asks for the xref name first, then its further answers.
void runOn(ui::PanelContext& ctx, const char* command, const std::string& name,
           std::initializer_list<const char*> answers = {}) {
    if (!ctx.runCommand)
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    ctx.runCommand(command);
    ctx.runCommand(name);
    for (const char* a : answers)
        ctx.runCommand(a);
}

void drawXrefPanel(ui::PanelContext& ctx) {
    if (!ctx.doc)
        return;
    XrefPanelState& st = state();
    const Document& doc = *ctx.doc;
    std::filesystem::path hostFile;
    if (ctx.host)
        if (auto f = ctx.host->setting("document.file"))
            hostFile = std::filesystem::path(std::u8string(f->begin(), f->end()));
    const std::vector<BlockId> ids = xref::xrefs(doc);

    if (ImGui::Button("Attach...") && ctx.runCommand) {
        if (ctx.cancelCommand)
            ctx.cancelCommand();
        ctx.runCommand("XATTACH");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload all") && ctx.runCommand && !ids.empty()) {
        if (ctx.cancelCommand)
            ctx.cancelCommand();
        ctx.runCommand("XRELOAD");
        ctx.runCommand("All");
    }
    ImGui::Separator();
    if (ids.empty()) {
        ImGui::TextDisabled("No external references in this drawing.");
        return;
    }

    const BlockDef* sel = st.selected ? doc.block(st.selected) : nullptr;
    if (sel && sel->xrefPath.empty())
        sel = nullptr;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 4.5f;
    if (ImGui::BeginTable("##xrefs", 5,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders,
                          ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Saved path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (BlockId id : ids) {
            const BlockDef* d = doc.block(id);
            if (!d)
                continue;
            const xref::State s = xref::state(doc, id, hostFile);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(id.value));
            if (ImGui::Selectable(d->name.c_str(), st.selected == id,
                                  ImGuiSelectableFlags_SpanAllColumns))
                st.selected = id;
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            if (s == xref::State::Missing)
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", stateLabel(s));
            else if (s == xref::State::Changed)
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "%s", stateLabel(s));
            else
                ImGui::TextUnformatted(stateLabel(s));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(typeLabel(*d));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", xref::useCount(doc, id));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(d->xrefPath.c_str());
        }
        ImGui::EndTable();
    }

    if (!sel) {
        ImGui::TextDisabled("Select a reference to reload, unload, detach, bind or relink it.");
        return;
    }
    // Copies: the buttons below run commands that rewrite or remove this block, so `sel` must not
    // be used after them.
    const std::string name = sel->name;
    const BlockId selId = sel->id;
    const xref::State s = xref::state(doc, selId, hostFile);
    if (ImGui::Button("Reload"))
        runOn(ctx, "XRELOAD", name);
    ImGui::SameLine();
    if (ImGui::Button("Unload"))
        runOn(ctx, "XUNLOAD", name);
    ImGui::SameLine();
    if (ImGui::Button("Detach"))
        runOn(ctx, "XDETACH", name);
    ImGui::SameLine();
    if (ImGui::Button("Bind"))
        runOn(ctx, "XBIND", name, {"Bind"});
    ImGui::SameLine();
    if (ImGui::Button("Insert (merge layers)"))
        runOn(ctx, "XBIND", name, {"Insert"});
    if (ImGui::BeginCombo("Path type", "Change...")) {
        for (const char* t : {"Relative", "Absolute", "None"})
            if (ImGui::Selectable(t))
                runOn(ctx, "XPATH", name, {t});
        ImGui::EndCombo();
    }
    if (st.relinkFor != selId) {
        st.relinkFor = selId;
        st.relink.clear();
    }
    if (s == xref::State::Missing)
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "File not found. Give its new location:");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##relink", "New file for this reference", &st.relink);
    ImGui::SameLine();
    if (ImGui::Button("Relink") && !st.relink.empty()) {
        runOn(ctx, "XRELINK", name, {st.relink.c_str()});
        st.relink.clear();
    }
}

}  // namespace

#endif

void registerXrefPanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("xrefs", "External References", drawXrefPanel, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
