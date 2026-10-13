// The "Text" panel: a multiline ImGui editor for the selected TEXT / MTEXT object. It uses the
// platform's IME like any other ImGui input, so Thai can be typed with the Windows keyboard
// layout or IME. Apply runs TEXTEDIT, so the edit is one undo step.
#include <algorithm>
#include <string>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "commands.h"
#include "tbcad/textcmd/textcmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

namespace {

struct EditorState {
    EntityId id{};
    std::string stored;  // what the entity contained when the editor last loaded it
    std::string buffer;  // editable form (MTEXT: "\P" shown as a newline)
    std::string wrapOpen;  // markup to wrap around the selection on the next callback
};

EditorState g_state;

int wrapCallback(ImGuiInputTextCallbackData* d) {
    auto* st = static_cast<EditorState*>(d->UserData);
    if (st->wrapOpen.empty())
        return 0;
    int a = std::min(d->SelectionStart, d->SelectionEnd);
    int b = std::max(d->SelectionStart, d->SelectionEnd);
    d->InsertChars(b, "}");
    d->InsertChars(a, st->wrapOpen.c_str());
    d->CursorPos = a + static_cast<int>(st->wrapOpen.size()) + (b - a);
    d->SelectionStart = d->SelectionEnd = d->CursorPos;
    st->wrapOpen.clear();
    return 0;
}

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
        if (ImGui::SmallButton("B"))
            g_state.wrapOpen = "{\\b1;";
        ImGui::SameLine();
        if (ImGui::SmallButton("I"))
            g_state.wrapOpen = "{\\i1;";
        ImGui::SameLine();
        if (ImGui::SmallButton("Height x2"))
            g_state.wrapOpen = "{\\H2x;";
        ImGui::SameLine();
        ImGui::TextDisabled("(wraps the selected text)");
    }
    ImVec2 size(-FLT_MIN, ImGui::GetTextLineHeight() * (isMText ? 10 : 3));
    ImGui::InputTextMultiline("##text", &g_state.buffer, size, ImGuiInputTextFlags_CallbackAlways,
                              wrapCallback, &g_state);
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
