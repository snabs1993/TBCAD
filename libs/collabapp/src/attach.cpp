#include <memory>

#include "collab_controller.h"
#include "collab_panel.h"
#include "tbcad/cmd/command.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/collabapp/collabapp.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::collabapp {

namespace {

class CollabCommand final : public cmd::Command {
public:
    explicit CollabCommand(ui::AppCore& core) : core_(core) {}
    std::string_view name() const override { return "COLLAB"; }
    std::string_view description() const override {
        return "Share this drawing with other people, or join one (live editing)";
    }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext&) override {
        core_.openPanel("collab");
        co_return;
    }

private:
    ui::AppCore& core_;
};

}  // namespace

std::function<void()> attach(ui::AppCore& core, cmd::Registry& commands, ui::UiRegistry& ui,
                             std::function<void()> wake) {
    auto controller = std::make_shared<CollabController>(core, std::move(wake));
    (void)commands.add(std::make_unique<CollabCommand>(core));
    ui.addMenuItem("File/Work Together (Share or Join)...", "COLLAB");
#ifdef TBCAD_COLLABAPP_IMGUI
    ui.addPanel(
        "collab", "Work Together",
        [controller](ui::PanelContext& ctx) { drawCollabPanel(*controller, ctx); }, false);
#endif
    ui.addStatusToggle(
        "collab", "LIVE",
        [controller] { return controller->mode() != CollabController::Mode::Off; },
        [controller, &core](bool on) {
            if (on)
                core.openPanel("collab");  // starting needs the panel: share or join
            else
                controller->stop();
        });
    ui.addOverlayDrawer([controller](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (controller->mode() == CollabController::Mode::Off)
            return;
        controller->shareCursor(ctx.cursorWorld);
        for (const CollabController::Cursor& c : controller->cursors())
            overlay.marker(render::Space::World, c.at, render::MarkerKind::Cross, 18,
                           CollabController::colorOf(c.peer), true, 2.5);
    });
    ui.addLabelProvider([controller](ui::PanelContext&, std::vector<ui::ViewportLabel>& out) {
        if (controller->mode() == CollabController::Mode::Off)
            return;
        const auto people = controller->peers();
        for (const CollabController::Cursor& c : controller->cursors()) {
            std::string name = c.peer == 0 ? "Host" : "Guest";
            for (const collab::PeerInfo& p : people)
                if (p.id == c.peer)
                    name = p.name;
            out.push_back({c.at, std::move(name), CollabController::colorOf(c.peer)});
        }
    });
    std::weak_ptr<CollabController> weak = controller;
    core.addDocumentClosingHook([weak](Document& doc) {
        if (auto c = weak.lock())
            c->drawingClosing(doc);
    });
    return [controller] { controller->pump(); };
}

}  // namespace tbcad::collabapp
