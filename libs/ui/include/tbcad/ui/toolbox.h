#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tbcad::ui {

// The Toolbox: the tabs, groups and tools shown under the menu bar. One table, no ImGui, so the
// layout can be tested without a window. A tool runs `command` (a command name or alias); a tool
// with `variants` shows a small arrow that opens the alternatives.
struct ToolVariant {
    std::string label;
    std::string command;  // command line to run
};

struct ToolDef {
    std::string label;    // short name under the button
    std::string command;  // command to run
    std::string
        icon;  // icon id (see iconFor in the app); empty or unknown draws the label's initials
    std::vector<ToolVariant> variants;
};

struct ToolGroup {
    std::string name;
    std::vector<ToolDef> tools;
};

struct ToolTab {
    std::string id;    // stable key saved in settings
    std::string name;  // tab title
    std::vector<ToolGroup> groups;
};

const std::vector<ToolTab>& toolboxTabs();

// The tool that runs `command` (first match across the tabs, command names compared as typed in
// capitals), or nullptr.
const ToolDef* findTool(std::string_view command);

// The Quick rail's commands until the user changes them.
std::vector<std::string> defaultQuickRail();

}  // namespace tbcad::ui
