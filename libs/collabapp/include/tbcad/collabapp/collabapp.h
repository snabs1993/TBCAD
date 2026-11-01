#pragma once

#include <functional>

namespace tbcad::cmd {
class Registry;
}
namespace tbcad::ui {
class AppCore;
class UiRegistry;
}  // namespace tbcad::ui

namespace tbcad::collabapp {

// Live editing of one drawing by several people on the same network (free; see
// docs/collaboration.md): the COLLAB command, the Collaboration panel, other people's cursors and
// names. `wake` may be called from any thread: it makes the window run one more loop turn. The
// returned function runs on the main thread once per loop turn.
std::function<void()> attach(ui::AppCore& core, cmd::Registry& commands, ui::UiRegistry& ui,
                             std::function<void()> wake);

}  // namespace tbcad::collabapp
