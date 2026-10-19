// The "Text Styles" manager: a list of the drawing's text styles with Set current, Rename, Delete
// and New, the font / width / oblique editors and a preview line drawn with the real text layout.
// Every edit runs a -STYLE command line (one undo step each), so the panel teaches the command
// line. Opened by TEXTSTYLES.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "commands.h"
#include "tbcad/geom/entity.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

namespace {

struct PanelState {
    TextStyleId selected{};
    std::string newName{};
    std::string renameTo{};
    std::string font{};  // editor buffer
    TextStyleId fontFor{};
    std::string sample = "TBCAD  สวัสดี  ABC abc 123";
};

PanelState& state() {
    static PanelState s;
    return s;
}

// Feeds a command line and its answers one line at a time (names may contain spaces). The lines
// end the command themselves ("Done"), so no extra Enter is sent.
void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    for (const std::string& l : lines)
        pc.runCommand(l);
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

std::string fontOf(const TextStyle& s) {
    return !s.fontFile.empty() ? s.fontFile : s.fontFamily;
}

void drawPreview(ui::PanelContext& pc, const TextStyle& style, const std::string& sample) {
    TextData t;
    t.height = 5;
    t.style = style.id;
    t.utf8 = sample;
    Entity e;
    e.data = Indirect<TextData>(t);
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.02;  // fine enough that the glyphs are outlines, not placeholder boxes
    DisplayList out;
    text::buildTextDisplay(*pc.doc, e, ctx, out);

    BBox2 box;
    for (const auto& item : out.items)
        if (const auto* f = std::get_if<DisplayFill>(&item))
            for (const auto& ring : f->rings)
                for (const Vec2& p : ring)
                    box.expand(p);

    ImVec2 size(std::max(280.0f, ImGui::GetContentRegionAvail().x - 4), 80);
    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
    if (!box.empty() && box.width() > 0 && box.height() > 0) {
        const float pad = 10;
        const double k =
            std::min((size.x - 2 * pad) / box.width(), (size.y - 2 * pad) / box.height());
        const Vec2 c = (box.min + box.max) / 2;
        auto map = [&](Vec2 p) {
            return ImVec2(static_cast<float>(min.x + size.x / 2 + (p.x - c.x) * k),
                          static_cast<float>(min.y + size.y / 2 - (p.y - c.y) * k));
        };
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        std::vector<ImVec2> pts;
        for (const auto& item : out.items) {
            const auto* f = std::get_if<DisplayFill>(&item);
            if (!f)
                continue;
            for (const auto& ring : f->rings) {
                pts.clear();
                for (const Vec2& p : ring)
                    pts.push_back(map(p));
                if (pts.size() >= 2)
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col,
                                    ImDrawFlags_Closed, 1.0f);
            }
        }
    }
    ImGui::Dummy(size);
}

void drawEditor(ui::PanelContext& pc, const TextStyle s) {  // by value: fields run -STYLE
    PanelState& st = state();
    const std::string head = "-STYLE";
    const bool isStandard = s.id == kTextStyleStandard;

    if (st.fontFor != s.id) {
        st.fontFor = s.id;
        st.font = fontOf(s);
    }
    ImGui::SeparatorText("Font");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##font", "Family name or font file (empty = Sarabun)", &st.font,
                             ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemDeactivatedAfterEdit() && st.font != fontOf(s))
        send(pc,
             {head, "Edit", s.name, "Font", st.font.empty() ? "default" : st.font, "Done", "Done"});
    if (!text::fontAvailable(s.fontFamily, s.fontFile))
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "Font not found: the bundled Sarabun is used.");

    ImGui::SeparatorText("Shape");
    double w = s.widthFactor;
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble("Width factor", &w, 0, 0, "%.4g", ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && w != s.widthFactor)
        send(pc, {head, "Edit", s.name, "Width", number(w), "Done", "Done"});
    double ob = s.oblique * 180.0 / kPi;
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble("Oblique angle", &ob, 0, 0, "%.4g", ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && std::abs(ob - s.oblique * 180.0 / kPi) > 1e-9)
        send(pc, {head, "Edit", s.name, "Oblique", number(ob), "Done", "Done"});

    double fh = s.height;
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble("Fixed height (0 = variable)", &fh, 0, 0, "%.4g",
                       ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && fh != s.height && fh >= 0)
        send(pc, {head, "Edit", s.name, "Height", number(fh), "Done", "Done"});

    ImGui::SeparatorText("Preview");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##sample", &st.sample);
    drawPreview(pc, s, st.sample);
    (void)isStandard;
}

void drawPanel(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    PanelState& st = state();
    const TextStyleId current = pc.doc->header().currentTextStyle;
    if (!pc.doc->textStyle(st.selected))
        st.selected = current;

    ImGui::BeginChild("textstyle-list", ImVec2(170, 0), ImGuiChildFlags_Borders);
    for (const auto& [id, s] : pc.doc->textStyles()) {
        const std::string label = s.name + (id == current ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), id == st.selected))
            st.selected = id;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    // A copy: Delete, Rename and the editor's fields run -STYLE, which replaces or removes the
    // row while this frame still draws it.
    std::optional<TextStyle> selRow;
    if (const TextStyle* found = pc.doc->textStyle(st.selected))
        selRow = *found;
    const TextStyle* sel = selRow ? &*selRow : nullptr;
    if (sel) {
        const bool isStandard = sel->id == kTextStyleStandard;
        ImGui::BeginDisabled(sel->id == current);
        if (ImGui::Button("Set current"))
            send(pc, {"-STYLE", "Current", sel->name, "Done"});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(sel->id == current || isStandard);
        if (ImGui::Button("Delete"))
            send(pc, {"-STYLE", "Delete", sel->name, "Done"});
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && sel->id != current &&
            !isStandard)
            ImGui::SetTooltip("Styles that text or dimension styles still use cannot be deleted.");

        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##rename", "New name", &st.renameTo);
        ImGui::SameLine();
        ImGui::BeginDisabled(isStandard || st.renameTo.empty() ||
                             pc.doc->textStyleByName(st.renameTo) != nullptr);
        if (ImGui::Button("Rename")) {
            send(pc, {"-STYLE", "Rename", sel->name, st.renameTo, "Done"});
            st.renameTo.clear();
        }
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##newstyle", "Name of the new style", &st.newName);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.newName.empty() || pc.doc->textStyleByName(st.newName) != nullptr);
    if (ImGui::Button("New from current")) {
        send(pc, {"-STYLE", "New", st.newName, "Done"});
        st.newName.clear();
        st.selected = {};
    }
    ImGui::EndDisabled();
    if (sel)
        drawEditor(pc, *sel);
    ImGui::EndGroup();
}

}  // namespace

void registerStylePanel(ui::UiRegistry& ui) {
    ui.addPanel("textstyles", "Text Styles", drawPanel, false);
}

}  // namespace tbcad::textcmd
