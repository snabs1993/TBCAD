#include "tbcad/textcmd/format_bar.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

#include "tbcad/core/color.h"
#include "tbcad/textcmd/textcmd.h"

namespace tbcad::textcmd {

namespace {

// AutoCAD's Symbol menu, plus Thai marks that are awkward to type on some keyboards.
constexpr std::pair<const char*, const char*> kSymbols[] = {
    {"Degree  \xC2\xB0", "\xC2\xB0"},
    {"Plus/minus  \xC2\xB1", "\xC2\xB1"},
    {"Diameter  \xC3\x98", "\xC3\x98"},
    {"Almost equal  \xE2\x89\x88", "\xE2\x89\x88"},
    {"Angle  \xE2\x88\xA0", "\xE2\x88\xA0"},
    {"Delta  \xCE\x94", "\xCE\x94"},
    {"Identity  \xE2\x89\xA1", "\xE2\x89\xA1"},
    {"Not equal  \xE2\x89\xA0", "\xE2\x89\xA0"},
    {"Ohm  \xE2\x84\xA6", "\xE2\x84\xA6"},
    {"Centre line  \xE2\x84\x84", "\xE2\x84\x84"},
    {"Property line  \xE2\x85\x8A", "\xE2\x85\x8A"},
    {"Squared  \xC2\xB2", "\xC2\xB2"},
    {"Cubed  \xC2\xB3", "\xC2\xB3"},
    {"Subscript 2  \xE2\x82\x82", "\xE2\x82\x82"},
    {"Non-breaking space", "\\~"},
    {"Baht  \xE0\xB8\xBF", "\xE0\xB8\xBF"},
    {"Thai repetition  \xE0\xB9\x86", "\xE0\xB9\x86"},
    {"Thai abbreviation  \xE0\xB8\xAF", "\xE0\xB8\xAF"},
};

TextSel fromData(const ImGuiInputTextCallbackData* d) {
    TextSel s;
    s.text.assign(d->Buf, static_cast<std::size_t>(d->BufTextLen));
    if (d->SelectionStart != d->SelectionEnd) {
        s.start = static_cast<std::size_t>(std::min(d->SelectionStart, d->SelectionEnd));
        s.end = static_cast<std::size_t>(std::max(d->SelectionStart, d->SelectionEnd));
    } else {
        s.start = s.end = static_cast<std::size_t>(d->CursorPos);
    }
    return s;
}

}  // namespace

void FormatBar::queueWrap(std::string open, std::string close) {
    op_ = Op::Wrap;
    a_ = std::move(open);
    b_ = std::move(close);
}

void FormatBar::queueInsert(std::string text) {
    op_ = Op::Insert;
    a_ = std::move(text);
}

void FormatBar::queueList(ListKind kind, double indent) {
    op_ = Op::List;
    list_ = kind;
    indent_ = indent;
}

bool FormatBar::apply(TextSel& s) {
    const Op op = std::exchange(op_, Op::None);
    switch (op) {
        case Op::None:
            return false;
        case Op::Wrap:
            s = wrapSelection(std::move(s), a_, b_);
            return true;
        case Op::Insert:
            s = insertText(std::move(s), a_);
            return true;
        case Op::Stack:
            return stackSelection(s);
        case Op::List:
            s = makeList(std::move(s), list_, indent_);
            return true;
        case Op::Paragraph:
            s = setParagraphCode(std::move(s), a_);
            return true;
        case Op::Replace: {
            std::size_t n = 0;
            s.text = replaceAll(s.text, find_, with_, matchCase_, n);
            s.start = s.end = std::min(s.start, s.text.size());
            replaced_ = std::to_string(n) + " replaced";
            return n > 0;
        }
    }
    return false;
}

int FormatBar::callback(ImGuiInputTextCallbackData* d) {
    auto* bar = static_cast<FormatBar*>(d->UserData);
    if (!bar)
        return 0;
    TextSel s = fromData(d);
    if (bar->op_ == Op::None) {
        // Remember the selection: a field of the bar used before a button (the height, the
        // colour number) takes over ImGui's text state, and the box comes back without it.
        bar->lastStart_ = s.start;
        bar->lastEnd_ = s.end;
        bar->lastLen_ = s.text.size();
        return 0;
    }
    if (bar->lastLen_ == s.text.size()) {
        s.start = bar->lastStart_;
        s.end = bar->lastEnd_;
    }
    bar->lastLen_ = std::string::npos;
    if (!bar->apply(s))
        return 0;
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, s.text.c_str());
    d->SelectionStart = static_cast<int>(s.start);
    d->SelectionEnd = static_cast<int>(s.end);
    d->CursorPos = static_cast<int>(s.end);
    return 0;
}

void FormatBar::draw(const Document& doc, const std::string& buffer, double height) {
    ImGui::PushID(this);
    auto button = [](const char* label, const char* tip) {
        const bool hit = ImGui::SmallButton(label);
        if (tip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return hit;
    };
    // Character format: wraps the selected text (or starts formatted text at the cursor).
    if (button("B", "Bold"))
        queueWrap("{\\b1;");
    if (button("I", "Italic"))
        queueWrap("{\\i1;");
    if (button("U", "Underline"))
        queueWrap("{\\L");
    if (button("O", "Overline"))
        queueWrap("{\\O");
    if (button("S", "Strikethrough"))
        queueWrap("{\\K");
    if (button("a/b", "Stack the selected fraction (1/2, 1#2 or 1^2)"))
        op_ = Op::Stack;

    // Colour of the selected text.
    ImGui::SetNextItemWidth(70);
    if (ImGui::BeginCombo("##colour", "Colour", ImGuiComboFlags_HeightLarge)) {
        static const char* const kNames[] = {"Red",     "Yellow", "Green", "Cyan",      "Blue",
                                             "Magenta", "White",  "Grey",  "Light grey"};
        for (int i = 1; i <= 9; ++i) {
            const Rgba c = paletteColor(static_cast<std::uint8_t>(i));
            ImGui::ColorButton(("##sw" + std::to_string(i)).c_str(),
                               ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine();
            if (ImGui::Selectable(kNames[i - 1]))
                queueWrap("{\\C" + std::to_string(i) + ";");
        }
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("##aci", &colour_, 0);
        colour_ = std::clamp(colour_, 1, 255);
        ImGui::SameLine();
        if (ImGui::SmallButton("Colour number")) {
            queueWrap("{\\C" + std::to_string(colour_) + ";");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // Height of the selected text.
    if (selHeight_ <= 0)
        selHeight_ = height > 0 ? height : 2.5;
    ImGui::SetNextItemWidth(54);
    ImGui::InputDouble("##selh", &selHeight_, 0, 0, "%.4g");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Height for the selected text");
    ImGui::SameLine();
    if (button("Set height", "Give the selected text this height") && selHeight_ > 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "{\\H%.6g;", selHeight_);
        queueWrap(buf);
    }

    // Symbols.
    if (ImGui::SmallButton("@ Symbol"))
        ImGui::OpenPopup("##symbols");
    if (ImGui::BeginPopup("##symbols")) {
        for (const auto& [label, text] : kSymbols)
            if (ImGui::Selectable(label))
                queueInsert(text);
        ImGui::EndPopup();
    }

    // Second row: paragraph.
    for (const auto& [label, code] : {std::pair<const char*, const char*>{"Left", "\\pxql;"},
                                      {"Centre", "\\pxqc;"},
                                      {"Right", "\\pxqr;"},
                                      {"Justify", "\\pxqj;"}})
        if (button(label, "Align this paragraph")) {
            op_ = Op::Paragraph;
            a_ = code;
        }
    const double indent = (height > 0 ? height : 2.5) * 2;
    if (ImGui::SmallButton("Bullets and numbering"))
        ImGui::OpenPopup("##lists");
    ImGui::SameLine();
    if (ImGui::BeginPopup("##lists")) {
        if (ImGui::Selectable("\xE2\x80\xA2 Bulleted"))
            queueList(ListKind::Bullets, indent);
        if (ImGui::Selectable("1. Numbered"))
            queueList(ListKind::Numbers, indent);
        if (ImGui::Selectable("a. Lettered"))
            queueList(ListKind::Letters, indent);
        if (ImGui::Selectable("Off"))
            queueList(ListKind::Off, indent);
        ImGui::EndPopup();
    }
    if (ImGui::SmallButton("Paragraph..."))
        ImGui::OpenPopup("##paragraph");
    ImGui::SameLine();
    if (ImGui::BeginPopup("##paragraph")) {
        ImGui::TextDisabled("Lengths in drawing units");
        auto field = [](const char* label, double& v) {
            ImGui::SetNextItemWidth(80);
            ImGui::InputDouble(label, &v, 0, 0, "%.4g");
        };
        field("First line indent", first_);
        field("Left indent", left_);
        field("Right indent", right_);
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("Tab stops", "10,20,30", &tabs_);
        field("Space before", before_);
        field("Space after", after_);
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("Alignment", &align_, "As attached\0Left\0Centre\0Right\0Justify\0");
        if (ImGui::Button("Apply to this paragraph")) {
            std::vector<double> tabs;
            std::string t = tabs_;
            std::replace(t.begin(), t.end(), ',', ' ');
            std::istringstream in(t);
            for (double v; in >> v;)
                if (v > 0)
                    tabs.push_back(v);
            std::sort(tabs.begin(), tabs.end());
            static const char* const kAlign[] = {"", "l", "c", "r", "j"};
            op_ = Op::Paragraph;
            a_ = paragraphCode(first_, left_, right_, tabs, before_, after_, kAlign[align_]);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::SmallButton("Find and replace"))
        ImGui::OpenPopup("##findreplace");
    ImGui::SameLine();
    if (ImGui::BeginPopup("##findreplace")) {
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Find", &find_);
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Replace with", &with_);
        ImGui::Checkbox("Match case", &matchCase_);
        std::size_t n = 0;
        (void)replaceAll(buffer, find_, with_, matchCase_, n);
        ImGui::TextDisabled("%zu found", n);
        ImGui::BeginDisabled(find_.empty() || n == 0);
        if (ImGui::Button("Replace all"))
            op_ = Op::Replace;
        ImGui::EndDisabled();
        if (!replaced_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", replaced_.c_str());
        }
        ImGui::EndPopup();
    }
    if (ImGui::SmallButton("Spelling"))
        ImGui::OpenPopup("##spelling");
    if (ImGui::BeginPopup("##spelling")) {
        const auto issues = misspelledWords(doc, editableToMarkup(buffer));
        if (issues.empty())
            ImGui::TextUnformatted("No spelling mistakes found.");
        for (const SpellIssue& is : issues) {
            ImGui::TextUnformatted(is.word.c_str());
            for (const std::string& sug : is.suggestions) {
                ImGui::SameLine();
                if (ImGui::SmallButton(sug.c_str())) {
                    find_ = is.word;
                    with_ = sug;
                    matchCase_ = true;
                    op_ = Op::Replace;
                }
            }
        }
        ImGui::TextDisabled("English words only; Thai text is not checked.");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

}  // namespace tbcad::textcmd
