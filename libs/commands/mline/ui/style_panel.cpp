// The Multiline Style dialog (MLSTYLE): the styles, Set current / New / Delete, a preview, and
// the lines (offsets, colours, linetypes), caps, arcs, fill and joints of the selected one.
// Every change runs -MLSTYLE, so it is one undo step and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/color.h"
#include "tbcad/core/document.h"
#include "tbcad/mline/mline.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::mlinecmd {

namespace {

struct MlStylePanel {
    std::string selected = "STANDARD";
    std::string newName{};
    std::string newOffsets = "0.5 -0.5";
    // Text boxes of the selected style; reloaded when the selection or the style changes.
    std::string shown{};
    std::string offsets{}, colors{}, linetypes{}, description{};
};

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

std::string joined(const mline::Style& s, const char* what) {
    std::string out;
    for (std::size_t i = 0; i < s.offsets.size(); ++i) {
        std::string w = what[0] == 'o'            ? num(s.offsets[i])
                        : what[0] == 'c'          ? std::to_string(s.colorOf(i))
                        : s.linetypeOf(i).empty() ? std::string("-")
                                                  : s.linetypeOf(i);
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

// The lines of the style drawn along a short horizontal path, with caps, arcs and fill.
void drawPreview(const mline::Style& s) {
    const ImVec2 size(240, 80);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(255, 255, 255, 255));
    if (s.offsets.empty()) {
        ImGui::Dummy(size);
        return;
    }
    const auto [lo, hi] = std::minmax_element(s.offsets.begin(), s.offsets.end());
    const double span = std::max(*hi - *lo, 1e-9);
    const float scale = static_cast<float>((size.y - 24) / span);
    const float midY = p.y + size.y / 2;
    const double mid = (*hi + *lo) / 2;
    auto yOf = [&](double o) { return midY - static_cast<float>((o - mid) * scale); };
    const float x0 = p.x + 30, x1 = p.x + size.x - 30;
    const ImU32 ink = IM_COL32(40, 40, 40, 255);
    auto colorOf = [&](int c) {
        if (c < 1 || c > 255)
            return ink;
        const Rgba rgba = paletteColor(static_cast<std::uint8_t>(c));
        return IM_COL32(rgba.r, rgba.g, rgba.b, 255);
    };
    if (s.fill >= 0)
        dl->AddRectFilled(ImVec2(x0, yOf(*hi)), ImVec2(x1, yOf(*lo)),
                          (colorOf(s.fill) & 0x00FFFFFF) | 0x60000000);
    for (std::size_t i = 0; i < s.offsets.size(); ++i) {
        const float y = yOf(s.offsets[i]);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), colorOf(s.colorOf(i)), 1.5f);
    }
    if (s.startCap)
        dl->AddLine(ImVec2(x0, yOf(*hi)), ImVec2(x0, yOf(*lo)), ink, 1.5f);
    if (s.endCap)
        dl->AddLine(ImVec2(x1, yOf(*hi)), ImVec2(x1, yOf(*lo)), ink, 1.5f);
    const float r = (yOf(*lo) - yOf(*hi)) / 2;
    if (s.startArc && r > 1) {
        dl->PathArcTo(ImVec2(x0, midY), r, 1.5708f, 4.7124f);
        dl->PathStroke(ink, 0, 1.5f);
    }
    if (s.endArc && r > 1) {
        dl->PathArcTo(ImVec2(x1, midY), r, -1.5708f, 1.5708f);
        dl->PathStroke(ink, 0, 1.5f);
    }
    ImGui::Dummy(size);
}

const char* arcWord(int a) {
    return a == 1 ? "Outer" : a == 2 ? "Inner" : "None";
}

// A text box that runs -MLSTYLE <keyword> <style> <text> when it loses focus after an edit.
void textField(ui::PanelContext& pc, const char* label, const char* keyword,
               const std::string& style, std::string& buf, const std::string& was) {
    ImGui::SetNextItemWidth(200);
    ImGui::InputText(label, &buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && buf != was)
        send(pc, {"-MLSTYLE", keyword, style, buf});
}

void drawMlStyles(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static MlStylePanel st;
    const Document& doc = *pc.doc;
    const std::vector<std::string> names = mline::styleNames(doc);
    std::string current = "STANDARD";
    if (pc.host)
        current = pc.host->setting("mline.style").value_or("STANDARD");
    auto has = [&](const std::string& n) {
        return std::any_of(names.begin(), names.end(),
                           [&](const std::string& x) { return lower(x) == lower(n); });
    };
    if (!has(st.selected))
        st.selected = has(current) ? current : std::string("STANDARD");

    ImGui::BeginChild("mls-list", ImVec2(160, 0), ImGuiChildFlags_Borders);
    for (const std::string& n : names) {
        const bool cur = lower(n) == lower(current);
        const std::string label = n + (cur ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), lower(n) == lower(st.selected)))
            st.selected = n;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const mline::Style s = mline::findStyle(doc, st.selected).value_or(mline::standardStyle());
    const std::string sel = s.name;
    const std::string sig = sel + "\n" + mline::encode(s);
    if (st.shown != sig) {
        st.shown = sig;
        st.offsets = joined(s, "offsets");
        st.colors = joined(s, "colors");
        st.linetypes = joined(s, "linetypes");
        st.description = s.description;
    }
    ImGui::BeginDisabled(pc.commandActive);
    ImGui::BeginDisabled(lower(sel) == lower(current));
    if (ImGui::Button("Set current"))
        send(pc, {"-MLSTYLE", "Current", sel});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(lower(sel) == "standard");
    if (ImGui::Button("Delete"))
        send(pc, {"-MLSTYLE", "Delete", sel});
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##mlsnew", "New style name", &st.newName);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputTextWithHint("##mlsnewoffs", "Offsets", &st.newOffsets);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.newName.empty() || st.newOffsets.empty());
    if (ImGui::Button("New")) {
        send(pc, {"-MLSTYLE", "New", st.newName, st.newOffsets, "None"});
        st.selected = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();

    drawPreview(s);
    textField(pc, "Description", "Description", sel, st.description, s.description);

    ImGui::SeparatorText("Lines");
    textField(pc, "Offsets", "Offsets", sel, st.offsets, joined(s, "offsets"));
    textField(pc, "Colours (256 = as the multiline)", "Colors", sel, st.colors,
              joined(s, "colors"));
    textField(pc, "Linetypes (- = as the multiline)", "Linetypes", sel, st.linetypes,
              joined(s, "linetypes"));

    ImGui::SeparatorText("Caps");
    const char* capWord = s.startCap && s.endCap ? "Both"
                          : s.startCap           ? "Start"
                          : s.endCap             ? "End"
                                                 : "None";
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("Line caps", capWord)) {
        for (const char* c : {"None", "Start", "End", "Both"})
            if (ImGui::Selectable(c, std::string(c) == capWord))
                send(pc, {"-MLSTYLE", "Caps", sel, c});
        ImGui::EndCombo();
    }
    int arcs[2] = {s.startArc, s.endArc};
    const char* arcLabels[2] = {"Arcs at start", "Arcs at end"};
    for (int i = 0; i < 2; ++i) {
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo(arcLabels[i], arcWord(arcs[i]))) {
            for (int a = 0; a <= 2; ++a)
                if (ImGui::Selectable(arcWord(a), a == arcs[i]) && a != arcs[i]) {
                    int next[2] = {arcs[0], arcs[1]};
                    next[i] = a;
                    send(pc, {"-MLSTYLE", "Arcs", sel, arcWord(next[0]), arcWord(next[1])});
                }
            ImGui::EndCombo();
        }
    }
    bool joints = s.joints;
    if (ImGui::Checkbox("Display joints", &joints))
        send(pc, {"-MLSTYLE", "Joints", sel, joints ? "On" : "Off"});

    ImGui::SeparatorText("Fill");
    bool fill = s.fill >= 0;
    if (ImGui::Checkbox("Fill between the outer lines", &fill))
        send(pc, {"-MLSTYLE", "Fill", sel, fill ? "256" : "None"});
    if (s.fill >= 0) {
        int c = s.fill;
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Fill colour (256 = as the multiline)", &c, 0);
        if (ImGui::IsItemDeactivatedAfterEdit() && c >= 0 && c <= 256 && c != s.fill)
            send(pc, {"-MLSTYLE", "Fill", sel, std::to_string(c)});
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

}  // namespace

void registerMlStylePanel(ui::UiRegistry& ui) {
    ui.addPanel("mlstyles", "Multiline Style", drawMlStyles, false);
}

}  // namespace tbcad::mlinecmd
