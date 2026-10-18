// The "Annotation" part of the Properties panel for text, dimensions, leaders, tables and
// symbols: whether the object follows an annotation scale, and the scales it is made for. Edits
// run ANNOTATIVE, so each is one undo step and the command line shows how to do it by hand.
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

#include "commands.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

namespace {

std::string scaleLabel(double n) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "1:%g", n);
    return buf;
}

struct PopupState {
    EntityId forId{};
    std::set<double> picked{};
    char custom[48] = "";
};

PopupState& popup() {
    static PopupState s;
    return s;
}

bool takesPart(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    return e && (canScaleAnnotatively(kind(*e)) || kind(*e) == EntityKind::Insert);
}

void drawSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc)
        return;
    // Every selected object that can be annotative; a single pick is the common case.
    std::vector<EntityId> ids;
    if (pc.multiSelection && pc.selection) {
        for (EntityId s : *pc.selection)
            if (takesPart(*pc.doc, s))
                ids.push_back(s);
        // One section for the whole selection, however many kinds it holds.
        if (ids.empty() || ids.front() != id)
            return;
    } else if (takesPart(*pc.doc, id)) {
        ids.push_back(id);
    }
    if (ids.empty())
        return;
    const Entity* e = pc.doc->find(id);
    const Header& h = pc.doc->header();
    const EntityCommon& c = e->common;
    auto run = [&](const std::vector<std::string>& lines) {
        if (!pc.runCommand)
            return;
        for (const std::string& l : lines)
            pc.runCommand(l);
    };
    bool allOn = true, anyOn = false, sameScales = true;
    for (EntityId i : ids) {
        const EntityCommon& ci = pc.doc->find(i)->common;
        allOn = allOn && ci.annotative;
        anyOn = anyOn || ci.annotative;
        if (!(ci.annoScales == c.annoScales))
            sameScales = false;
    }
    const bool mixedOn = anyOn && !allOn;

    ImGui::SeparatorText("Annotation");
    if (ids.size() > 1)
        ImGui::TextDisabled("%zu objects", ids.size());
    bool on = allOn;
    ImGui::BeginDisabled(pc.commandActive);
    if (ImGui::Checkbox(mixedOn ? "Annotative (*VARIES*)" : "Annotative", &on))
        run({"ANNOTATIVE", (mixedOn || on) ? "On" : "Off"});
    if (anyOn) {
        std::string label;
        if (!sameScales) {
            label = "*VARIES*";
        } else {
            if (c.annoScales.empty())
                label = scaleLabel(currentAnnotationScale(h)) + " (current)";
            for (double v : c.annoScales)
                label += (label.empty() ? "" : ", ") + scaleLabel(v);
        }
        ImGui::Text("Scale: %s", label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Change...")) {
            PopupState& s = popup();
            s.forId = id;
            s.picked.clear();
            for (double v : c.annoScales)
                s.picked.insert(v);
            s.custom[0] = 0;
            ImGui::OpenPopup("##annoscales");
        }
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopup("##annoscales")) {
        PopupState& s = popup();
        ImGui::TextDisabled("Scales this object is made for");
        for (double v : annotationScaleList(h)) {
            bool sel = s.picked.count(v) != 0;
            if (ImGui::Checkbox(scaleLabel(v).c_str(), &sel)) {
                if (sel)
                    s.picked.insert(v);
                else
                    s.picked.erase(v);
            }
        }
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##custom", "custom N", s.custom, sizeof s.custom);
        if (ImGui::Button("Apply") && s.forId == id) {
            std::string csv;
            for (double v : s.picked) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%g", v);
                csv += (csv.empty() ? "" : ",") + std::string(buf);
            }
            if (s.custom[0])
                csv += (csv.empty() ? "" : ",") + std::string(s.custom);
            run({"ANNOTATIVE", "Scales", csv});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

void registerAnnotationSection(ui::UiRegistry& ui) {
    for (EntityKind k : {EntityKind::Text, EntityKind::MText, EntityKind::Dimension,
                         EntityKind::Leader, EntityKind::MLeader, EntityKind::Table, EntityKind::Insert})
        ui.addPropertySection(k, drawSection);
}

}  // namespace tbcad::textcmd
