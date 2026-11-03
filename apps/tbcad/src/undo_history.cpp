#include "undo_history.h"

#include <imgui.h>

#include <string>

namespace tbcad::app {

namespace {

std::string rowText(const cmd::UndoStack::Step& s, std::size_t number) {
    std::string t = std::to_string(number) + "  " + s.label;
    if (s.objects > 0)
        t += "  -  " + std::to_string(s.objects) + (s.objects == 1 ? " object" : " objects");
    return t;
}

void draw(ui::AppCore& core, ui::PanelContext& pc) {
    cmd::UndoStack& st = core.undoStack();
    const auto done = st.undoSteps();
    const auto ahead = st.redoSteps();
    const long long here = static_cast<long long>(done.size());

    if (done.empty() && ahead.empty()) {
        ImGui::TextDisabled("Nothing to undo yet.");
        ImGui::TextWrapped("Every change you make is listed here. Click a row to go back to it.");
        return;
    }
    ImGui::TextDisabled("Click a step to go back or forward to it.");
    if (pc.commandActive)
        ImGui::TextDisabled("A click ends the command in progress.");

    long long target = -1;  // history depth to go to
    if (ImGui::BeginChild("##steps", ImVec2(0, 0), ImGuiChildFlags_None)) {
        const std::size_t dropped = st.droppedSteps();
        const char* startLabel = dropped > 0 ? "Oldest step kept" : "Start";
        if (ImGui::Selectable(startLabel, here == 0))
            target = 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(dropped > 0 ? "%zu older steps were let go to save memory"
                                          : "The drawing before the first change",
                              dropped);
        static std::size_t lastCount = static_cast<std::size_t>(-1);
        const std::size_t count = done.size() + ahead.size();
        for (std::size_t i = 0; i < count; ++i) {
            const bool isDone = i < done.size();
            const auto& s = isDone ? done[i] : ahead[i - done.size()];
            const bool current = static_cast<long long>(i) + 1 == here;
            ImGui::PushID(static_cast<int>(i));
            if (!isDone)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(rowText(s, dropped + i + 1).c_str(), current))
                target = static_cast<long long>(i) + 1;
            if (!isDone)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    isDone ? (current ? "You are here" : "Go back to just after this step")
                           : "Undone: click to redo up to this step");
            if (current && lastCount != count + static_cast<std::size_t>(here))
                ImGui::SetScrollHereY(0.5f);
            ImGui::PopID();
        }
        lastCount = count + static_cast<std::size_t>(here);
    }
    ImGui::EndChild();

    if (target >= 0 && target != here) {
        if (pc.commandActive && pc.cancelCommand)
            pc.cancelCommand();
        const std::size_t moved = core.stepHistory(target - here);
        if (moved > 0)
            core.print(target < here ? "Went back " + std::to_string(moved) + " step(s)."
                                     : "Went forward " + std::to_string(moved) + " step(s).");
        else if (!st.blockedReason().empty())
            core.print(st.blockedReason());
    }
}

}  // namespace

void registerUndoHistoryPanel(ui::AppCore& core, ui::UiRegistry& registry) {
    registry.addPanel("history", "Undo History", [&core](ui::PanelContext& pc) { draw(core, pc); });
}

}  // namespace tbcad::app
