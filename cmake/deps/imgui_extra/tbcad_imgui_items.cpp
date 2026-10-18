#include "tbcad_imgui_items.h"

#include <imgui_internal.h>

#include <unordered_map>

namespace tbcad::imgui_items {

namespace {
struct State {
    std::vector<Item> items;
    std::unordered_map<ImGuiID, std::string> labels;
};
State& state() {
    static State s;
    return s;
}
}  // namespace

void setRecording(ImGuiContext* ctx, bool on) {
    ctx->TestEngineHookItems = on;
}

void clear() {
    state().items.clear();
    state().labels.clear();
}

const std::vector<Item>& items() {
    State& s = state();
    for (Item& it : s.items) {
        if (it.label.empty()) {
            auto f = s.labels.find(it.id);
            if (f != s.labels.end())
                it.label = f->second;
        }
    }
    return s.items;
}

}  // namespace tbcad::imgui_items

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData*) {
    if (id == 0)
        return;
    tbcad::imgui_items::Item it;
    it.id = id;
    it.min = bb.Min;
    it.max = bb.Max;
    if (ctx->CurrentWindow)
        it.window = ctx->CurrentWindow->Name;
    tbcad::imgui_items::state().items.push_back(std::move(it));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (label)
        tbcad::imgui_items::state().labels[id] = label;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    auto& labels = tbcad::imgui_items::state().labels;
    auto f = labels.find(id);
    return f == labels.end() ? nullptr : f->second.c_str();
}
