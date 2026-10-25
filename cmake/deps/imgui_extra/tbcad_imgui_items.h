// Item recorder for automated GUI tests. Dear ImGui built with IMGUI_ENABLE_TEST_ENGINE calls the
// hook functions below for every widget; here they only write the widget's label and rectangle
// into a list, and only while a test has switched it on (ImGuiContext::TestEngineHookItems). The
// program itself never switches it on, so the cost is one untaken branch per widget.
#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace tbcad::imgui_items {

struct Item {
    ImGuiID id = 0;
    ImGuiID seed =
        0;  // top of the ID stack when the widget was added: id == ImHashStr(label, 0, seed)
    ImVec2 min{}, max{};   // screen rectangle
    std::string window{};  // name of the window it was added to
    std::string label{};   // from ImGuiTestEngineHook_ItemInfo, empty when the widget gave none
};

// Starts or stops recording for `ctx`.
void setRecording(ImGuiContext* ctx, bool on);
// Forgets the items; call before ImGui::NewFrame() to keep only the newest frame.
void clear();
const std::vector<Item>& items();

}  // namespace tbcad::imgui_items

// Cluster stepping in text boxes (see cmake/deps/imgui.cmake). Defined in the patched
// imgui_widgets.cpp, null until the program sets them. `next` returns the byte offset of the next
// cluster start after `idx` (idx < len); `prev` the one before `idx` (idx > 0). Both work on the
// UTF-8 text of the box.
extern int (*TbcadImGuiClusterNext)(const char* s, int len, int idx);
extern int (*TbcadImGuiClusterPrev)(const char* s, int len, int idx);
