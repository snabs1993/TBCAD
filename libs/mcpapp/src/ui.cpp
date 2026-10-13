#include <cstdio>

#include "tbcad/mcpapp/controller.h"

#ifdef TBCAD_MCPAPP_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::mcpapp {

void McpController::registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Tools/MCP Server", "MCPTOGGLE", "mcp.toggle");
    ui.addMenuItem("Tools/MCP Settings...", "MCPSETTINGS");
    ui.addStatusToggle(
        "mcp", "MCP", [this] { return running(); }, [this](bool on) { setEnabled(on); });
    ui.addPanel("mcp", "MCP Server", [this](ui::PanelContext& ctx) { drawPanel(ctx); }, false);
}

#ifdef TBCAD_MCPAPP_IMGUI

namespace {

const char* policyLabel(EditPolicy p) {
    switch (p) {
        case EditPolicy::Allow:
            return "Allow edits";
        case EditPolicy::ReadOnly:
            return "Read only";
        default:
            return "Ask me each session";
    }
}

std::string clientLine(const mcp::ClientInfo& c) {
    return c.version.empty() ? c.name : c.name + " " + c.version;
}

}  // namespace

void McpController::drawPanel(ui::PanelContext&) {
    bool on = running();
    if (ImGui::Checkbox("MCP server on", &on))
        setEnabled(on);
    if (running()) {
        ImGui::TextDisabled("Listening on %s (this computer only)", url().c_str());
    } else if (!lastError().empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1), "%s", lastError().c_str());
    } else {
        ImGui::TextDisabled("Off. AI programs cannot see or change your drawing.");
    }

    ImGui::SeparatorText("Edit permission");
    EditPolicy p = policy();
    if (ImGui::BeginCombo("##policy", policyLabel(p))) {
        for (EditPolicy c : {EditPolicy::Ask, EditPolicy::Allow, EditPolicy::ReadOnly})
            if (ImGui::Selectable(policyLabel(c), c == p))
                setPolicy(c);
        ImGui::EndCombo();
    }
    if (portText_.empty())
        portText_ = std::to_string(configuredPort());
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputText("Port (applies when the server starts)", &portText_,
                         ImGuiInputTextFlags_CharsDecimal)) {
        try {
            setConfiguredPort(std::stoi(portText_));
        } catch (const std::exception&) {
        }
    }

    ImGui::SeparatorText("Connect an AI program");
    if (hooks_.bridgeExe.empty()) {
        ImGui::TextDisabled("tbcad-mcp.exe was not found next to TBCAD.");
    } else {
        if (ImGui::Button("Copy Claude Desktop config"))
            ImGui::SetClipboardText(claudeDesktopConfig().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Copy Claude Code command"))
            ImGui::SetClipboardText(claudeCodeCommand().c_str());
    }

    ImGui::SeparatorText("Connected");
    const auto list = sessions();
    if (list.empty())
        ImGui::TextDisabled("No AI program is connected.");
    for (const auto& s : list) {
        ImGui::PushID(s.id.c_str());
        const char* state = !s.decision                                  ? "waiting"
                            : *s.decision == mcp::EditDecision::Allow    ? "can edit"
                            : *s.decision == mcp::EditDecision::ReadOnly ? "read only"
                                                                         : "denied";
        ImGui::Text("%s  (%s)", clientLine({s.clientName, s.clientVersion, s.id}).c_str(), state);
        ImGui::SameLine();
        if (ImGui::SmallButton("Take away editing"))
            revokeEdit(s.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Disconnect"))
            closeSession(s.id);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Activity");
    const auto log = activity();
    if (ImGui::BeginChild("##mcpactivity", ImVec2(0, 0), true)) {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            const auto& c = it->call;
            if (c.ok)
                ImGui::Text("%s  %s  %.0f ms", c.client.c_str(), c.tool.c_str(), c.milliseconds);
            else
                ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1), "%s  %s  failed: %s",
                                   c.client.c_str(), c.tool.c_str(), c.error.c_str());
            if (!c.summary.empty())
                ImGui::TextDisabled("    %s", c.summary.c_str());
        }
    }
    ImGui::EndChild();
}

void McpController::drawDialogs() {
    auto req = pendingRequest();
    if (!req)
        return;
    ImGui::OpenPopup("MCP edit request");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(
            "MCP edit request", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("%s wants to change your drawing.", clientLine(req->client).c_str());
        ImGui::TextDisabled("Every change is one step you can undo. It asks again next session.");
        ImGui::Spacing();
        if (ImGui::Button("Allow")) {
            answerRequest(req->id, mcp::EditDecision::Allow);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Read only")) {
            answerRequest(req->id, mcp::EditDecision::ReadOnly);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Deny")) {
            answerRequest(req->id, mcp::EditDecision::Deny);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

#else

void McpController::drawPanel(ui::PanelContext&) {}
void McpController::drawDialogs() {}

#endif

}  // namespace tbcad::mcpapp
