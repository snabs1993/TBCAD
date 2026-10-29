// Drives the real Gui (menus, panels, dialogs, command line, viewport) without a window or a GPU:
// GLFW runs on its "null" platform, Dear ImGui gets its input as queued events, and the draw lists
// are painted by a small software rasteriser so a frame can be compared with a golden picture.
// Widgets are found by the label they were created with (tbcad_imgui_items.h).
#pragma once

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbcad_imgui_items.h"

struct GLFWwindow;

namespace tbcad {
namespace cmd {
class Registry;
}
namespace ui {
class AppCore;
class UiRegistry;
}  // namespace ui
namespace app {
class Gui;
}
}  // namespace tbcad

namespace tbcad::guitest {

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba{};  // 4 bytes a pixel, row 0 on top
    bool empty() const { return width == 0 || height == 0; }
};

// The visible part of a rectangle of the frame.
Image crop(const Image& img, int x, int y, int w, int h);

struct Tolerance {
    int channel = 24;         // a pixel differs when any colour channel is off by more than this
    double fraction = 0.004;  // share of differing pixels the picture may have
};

// Compares `img` with tests/golden/<name>.png. A mismatch writes <name>.actual.png and
// <name>.diff.png to the build folder and fails with their paths. With TBCAD_UPDATE_GOLDEN=1 in the
// environment the golden file is written instead (and the check passes).
// Writes `img` as <name>.png to the build folder's failure/preview directory; returns the path.
std::filesystem::path savePreview(const Image& img, const std::string& name);

bool matchesGolden(const Image& img, const std::string& name, const Tolerance& tol = {});

class Harness {
public:
    explicit Harness(int width = 1440, int height = 900);
    ~Harness();
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    ui::AppCore& core() { return *core_; }
    app::Gui& gui() { return *gui_; }
    const ui::UiRegistry& ui() const { return *uiRegistry_; }
    // Runs `n` frames (input queued so far is delivered on the first).
    void frame(int n = 1);
    // Frames until nothing changes for a few frames in a row (at most `limit`).
    void settle(int limit = 30);

    // ---- widgets: found by label in the newest frame
    // A label matches when it equals the item's visible text (the part before "##"), or the whole
    // label. `window` (when not empty) must be part of the window's name; `nth` picks among several
    // matches, top to bottom, left to right.
    const imgui_items::Item* find(const std::string& label, const std::string& window = {},
                                  int nth = 0) const;
    bool has(const std::string& label, const std::string& window = {}) const {
        return find(label, window) != nullptr;
    }
    std::vector<std::string> labels(const std::string& window = {}) const;  // for failure messages
    bool windowOpen(const std::string& windowPart) const;

    // Mouse and keyboard, delivered the way a person would: move, press, release, each in its own
    // frame so hover and active states are seen.
    void moveMouse(float x, float y);
    void click(float x, float y, int button = 0);
    void doubleClick(float x, float y);
    void drag(float x0, float y0, float x1, float y1, int button = 0);
    void scroll(float x, float y, float dy);
    // Clicks the centre of the widget; false (and a note in the failure log) when it is not there.
    bool click(const std::string& label, const std::string& window = {}, int nth = 0);
    bool doubleClickItem(const std::string& label, const std::string& window = {}, int nth = 0);
    bool rightClick(const std::string& label, const std::string& window = {}, int nth = 0);
    bool clickAt(const imgui_items::Item& item, int button = 0);
    // Hold Ctrl or Shift for the next clicks.
    void holdCtrl(bool on);
    void holdShift(bool on);
    // Types text into whatever has keyboard focus.
    void type(const std::string& utf8);
    void key(ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false);
    // Clicks a text field, replaces its content with `text` and presses Enter.
    bool enterText(const std::string& label, const std::string& text,
                   const std::string& window = {}, int nth = 0);
    // Types a line in the command window and presses Enter.
    void command(const std::string& line);

    // The current frame as a picture.
    Image screenshot();
    // A frame with `draw` painting on top of everything (into the draw list it is given), returned
    // as the top-left w x h pixels. For previews of things that have no place in the Gui yet.
    Image overlayShot(int w, int h, const std::function<void(ImDrawList*)>& draw);
    // The frame cut to the window whose name contains `windowPart` (with a small margin).
    Image windowShot(const std::string& windowPart);

    // ImGui's own error reports (unbalanced Begin / End, duplicate ids ...); the harness fails a
    // test that has any when it is destroyed.
    const std::vector<std::string>& imguiErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<cmd::Registry> registry_;
    std::unique_ptr<ui::UiRegistry> uiRegistry_;
    std::unique_ptr<ui::AppCore> core_;
    std::unique_ptr<app::Gui> gui_;
};

}  // namespace tbcad::guitest
