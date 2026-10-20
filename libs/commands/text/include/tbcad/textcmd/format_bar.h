#pragma once

// The formatting bar of the MTEXT editors (the in-place editor and the Text panel): character
// format, colour, height, stacking, symbols, lists, paragraph settings, find and replace, and a
// spelling check. Needs Dear ImGui, so it exists only in builds of the application.

#include <string>

#include "tbcad/core/document.h"
#include "tbcad/textcmd/format_edits.h"

struct ImGuiInputTextCallbackData;

namespace tbcad::textcmd {

class FormatBar {
public:
    // Draws the bar above an editor whose editable text is `buffer`. `height` is the text
    // height, used for list indents and offered as the height of selected text. A button only
    // queues its edit: ImGui owns the text while the box is active, so the box applies it on
    // its next frame through `callback`.
    void draw(const Document& doc, const std::string& buffer, double height);

    // Pass as the callback of the editor's InputTextMultiline, with this bar as user data and
    // ImGuiInputTextFlags_CallbackAlways.
    static int callback(ImGuiInputTextCallbackData* data);

    // Applies the queued edit to `s` (what callback does with the box's text); false when
    // nothing was queued. For editors that are not active and for tests.
    bool apply(TextSel& s);

    // An edit is queued: the editor should take the keyboard focus back so it is applied.
    bool pending() const { return op_ != Op::None; }

    // Queues an edit as the buttons do (for tests and scripted use).
    void queueWrap(std::string open, std::string close = "}");
    void queueInsert(std::string text);
    void queueList(ListKind kind, double indent);

private:
    enum class Op { None, Wrap, Insert, Stack, List, Paragraph, Replace };
    Op op_ = Op::None;
    std::string a_, b_;  // wrap: open / close; insert: text; paragraph: code
    ListKind list_ = ListKind::Off;
    double indent_ = 0;
    // Find and replace.
    std::string find_, with_;
    bool matchCase_ = false;
    std::string replaced_;  // the last result, shown under the fields
    // Paragraph dialog.
    double first_ = 0, left_ = 0, right_ = 0, before_ = 0, after_ = 0;
    std::string tabs_;
    int align_ = 0;
    // The box's selection on its last active frame.
    std::size_t lastStart_ = 0, lastEnd_ = 0, lastLen_ = std::string::npos;
    // Selected text height and colour.
    double selHeight_ = 0;
    int colour_ = 1;
};

}  // namespace tbcad::textcmd
