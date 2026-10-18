#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <set>

#include "panel_util.h"
#include "panels.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"

namespace tbcad::layers {

namespace {

using namespace ui_detail;

struct WalkState {
    std::string filter{};
    std::set<std::uint32_t> picked{};
    std::uint32_t anchor = 0;
    bool applied = false;  // the drawing currently shows only the picked layers
};

WalkState& walk() {
    static WalkState s;
    return s;
}

std::string pickedRefs(const WalkState& st) {
    std::string out;
    for (std::uint32_t v : st.picked)
        out += (out.empty() ? "" : ",") + idRef(LayerId{v});
    return out;
}

}  // namespace

void drawLayWalkPanel(tbcad::ui::PanelContext& ctx) {
    if (!ctx.doc)
        return;
    WalkState& st = walk();
    const Document& doc = *ctx.doc;
    const bool restoreOnExit =
        ctx.host && ctx.host->setting("laywalk.restore").value_or("1") != "0";

    ImGui::TextWrapped("Pick layers to see only those; the rest are hidden until you restore.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##walkfilter", "Filter (text, or name:S-* used ...)", &st.filter);

    std::string f = st.filter;
    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool expression = f.find(':') != std::string::npos;
    std::vector<LayerId> rows;
    for (LayerId id : layerTreeOrder(doc)) {
        if (!f.empty()) {
            if (expression ? !layerMatchesFilter(doc, id, st.filter) : [&] {
                    std::string p = layerPath(doc, id);
                    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return p.find(f) == std::string::npos;
                }())
                continue;
        }
        rows.push_back(id);
    }

    bool changed = false;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + 8;
    if (ImGui::BeginChild("##walklist", ImVec2(0, -footer), true)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LayerId id = rows[static_cast<std::size_t>(i)];
                const Layer* l = doc.layer(id);
                if (!l)
                    continue;
                ImGui::PushID(static_cast<int>(id.value));
                ImGui::Indent(layerDepth(doc, id) * 12.0f);
                const bool sel = st.picked.count(id.value) > 0;
                if (ImGui::Selectable(l->name.c_str(), sel)) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift && st.anchor) {
                        auto a = std::find(rows.begin(), rows.end(), LayerId{st.anchor});
                        auto b = std::find(rows.begin(), rows.end(), id);
                        if (a != rows.end() && b != rows.end()) {
                            if (a > b)
                                std::swap(a, b);
                            if (!io.KeyCtrl)
                                st.picked.clear();
                            for (auto it = a; it <= b; ++it)
                                st.picked.insert(it->value);
                        }
                    } else if (io.KeyCtrl) {
                        if (!st.picked.erase(id.value))
                            st.picked.insert(id.value);
                        st.anchor = id.value;
                    } else {
                        st.picked = {id.value};
                        st.anchor = id.value;
                    }
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", layerPath(doc, id).c_str());
                ImGui::Unindent(layerDepth(doc, id) * 12.0f);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    if (changed) {
        if (st.picked.empty()) {
            exec(ctx, {"-LAYWALK Restore"});
            st.applied = false;
        } else {
            exec(ctx, {"-LAYWALK Show", pickedRefs(st)});
            st.applied = true;
        }
    }

    bool keep = restoreOnExit;
    if (ImGui::Checkbox("Restore on exit", &keep) && ctx.host)
        ctx.host->setSetting("laywalk.restore", keep ? "1" : "0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Closing this window brings back the layers as they were.\n"
            "Off: the layers stay as shown.");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu layers picked", st.picked.size(), doc.layers().size());

    if (ImGui::Button("Restore now")) {
        exec(ctx, {"-LAYWALK Restore"});
        st.picked.clear();
        st.applied = false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.applied);
    if (ImGui::Button("Keep this view")) {
        exec(ctx, {"-LAYWALK Keep"});
        st.applied = false;
    }
    ImGui::EndDisabled();
}

}  // namespace tbcad::layers
