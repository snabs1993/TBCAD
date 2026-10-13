#include "tbcad/cmd/command.h"
#include "tbcad/mcpapp/controller.h"

namespace tbcad::mcpapp {

namespace {

class ToggleCommand final : public cmd::Command {
public:
    explicit ToggleCommand(McpController& c) : c_(c) {}
    std::string_view name() const override { return "MCPTOGGLE"; }
    std::string_view description() const override {
        return "Turn the built-in MCP server (AI access to the drawing) on or off";
    }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        c_.toggle();
        ctx.message(c_.running()             ? "MCP server on: " + c_.url()
                    : c_.lastError().empty() ? std::string("MCP server off")
                                             : "MCP server: " + c_.lastError());
        co_return;
    }

private:
    McpController& c_;
};

class SettingsCommand final : public cmd::Command {
public:
    explicit SettingsCommand(McpController& c) : c_(c) {}
    std::string_view name() const override { return "MCPSETTINGS"; }
    std::string_view description() const override { return "Show the MCP server settings panel"; }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext&) override {
        c_.requestSettings();
        co_return;
    }

private:
    McpController& c_;
};

}  // namespace

void McpController::registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ToggleCommand>(*this));
    (void)registry.add(std::make_unique<SettingsCommand>(*this));
}

}  // namespace tbcad::mcpapp
