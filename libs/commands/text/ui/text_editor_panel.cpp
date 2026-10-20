// The "Text" panel: a multiline ImGui editor for the selected TEXT / MTEXT object. It uses the
// platform's IME like any other ImGui input, so Thai can be typed with the Windows keyboard
// layout or IME. Apply runs TEXTEDIT, so the edit is one undo step.
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>

#include "commands.h"
#include "tbcad/textcmd/format_bar.h"
#include "tbcad/textcmd/textcmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

namespace {

struct EditorState {
    EntityId id{};
    std::string stored;  // what the entity contained when the editor last loaded it
    std::string buffer;  // editable form (MTEXT: "\P" shown as a newline)
};

EditorState g_state;
FormatBar g_bar;

const Entity* selectedText(ui::PanelContext& pc) {
    if (!pc.doc || !pc.selection || pc.selection->size() != 1)
        return nullptr;
    const Entity* e = pc.doc->find(pc.selection->front());
    if (!e)
        return nullptr;
    EntityKind k = kind(*e);
    return (k == EntityKind::Text || k == EntityKind::MText) ? e : nullptr;
}

void drawPanel(ui::PanelContext& pc) {
    const Entity* e = selectedText(pc);
    if (!e) {
        ImGui::TextWrapped("Select one text object to edit it here, or run TEXT / MTEXT.");
        return;
    }
    const bool isMText = kind(*e) == EntityKind::MText;
    const std::string& content =
        isMText ? entityAs<MTextData>(*e)->utf8 : entityAs<TextData>(*e)->utf8;
    // Reload when the selection changed or the object changed behind our back (undo, TEXTEDIT).
    if (g_state.id != e->common.id || g_state.stored != content) {
        g_state.id = e->common.id;
        g_state.stored = content;
        g_state.buffer = isMText ? markupToEditable(content) : content;
    }

    ImGui::TextUnformatted(isMText ? "Multiline text" : "Single-line text");
    if (isMText) {
        g_bar.draw(*pc.doc, g_state.buffer, entityAs<MTextData>(*e)->height);
        if (ImGui::SmallButton("Background / spacing...") && pc.runCommand)
            pc.runCommand("MTEXTFORMAT");
    }
    ImVec2 size(-FLT_MIN, ImGui::GetTextLineHeight() * (isMText ? 10 : 3));
    if (isMText) {
        ImGui::InputTextMultiline("##text", &g_state.buffer, size,
                                  ImGuiInputTextFlags_CallbackAlways, &FormatBar::callback, &g_bar);
        // A bar button took the focus: give it back so the box applies the edit.
        if (g_bar.pending() && !ImGui::IsItemActive())
            ImGui::ActivateItemByID(ImGui::GetItemID());
    } else {
        ImGui::InputTextMultiline("##text", &g_state.buffer, size);
    }
    if (ImGui::Button("Apply") && pc.runCommand) {
        std::string value = isMText ? editableToMarkup(g_state.buffer) : g_state.buffer;
        if (!isMText)
            std::replace(value.begin(), value.end(), '\n', ' ');
        pc.runCommand("TEXTEDIT");
        pc.runCommand(value);
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        g_state.buffer = isMText ? markupToEditable(content) : content;
}

}  // namespace

void registerEditorPanel(ui::UiRegistry& ui) {
    ui.addPanel("text_editor", "Text", drawPanel, false);
}

}  // namespace tbcad::textcmd
