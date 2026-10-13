#pragma once

#include <imgui.h>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "file_browser.h"
#include "tbcad/core/color.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

struct GLFWwindow;

namespace tbcad::app {

// Installs the platform hook that places the input method's candidate window at the text cursor.
void installImeHook(GLFWwindow* window);

// (Re)builds the UI font atlas: Latin (Lato) merged with Thai (Sarabun) at `scale`. Needs the
// OpenGL backend to be initialised; recreates the font texture.
void buildFonts(float scale);
// Colours and metrics for the chosen theme; `scale` is the DPI scale for sizes.
void applyTheme(bool dark, float scale);
// Where bundled assets live: next to the executable, then ./assets, then the source tree.
std::string findAsset(const std::string& relative);
// A file shipped next to the executable (THIRD_PARTY_NOTICES.txt), or empty.
std::string findBesideExecutable(const std::string& name);
// Drawing-area background for the theme.
Rgba backgroundFor(bool dark);

struct ViewportRect {
    float x = 0, y = 0, w = 0, h = 0;  // window points
};

// Everything drawn with Dear ImGui and the viewport mouse/keyboard handling. Owns no OpenGL
// objects; the App draws the scene into a target and the Gui puts it on screen.
class Gui {
public:
    Gui(ui::AppCore& core, ui::UiRegistry& registry, GLFWwindow* window);

    // Builds the whole UI for this frame. Returns the viewport rectangle (window points).
    ViewportRect frame();
    // Shows the rendered drawing in the viewport rectangle; call after frame(), before Render().
    void showViewport(unsigned texture, const ViewportRect& rect);

    bool wantsExit() const { return exitRequested_; }
    // The window's close button: asks to save first when needed. Returns true when it is fine to
    // close right now.
    bool requestClose();
    // True after input or a visible change; the App keeps drawing for a few frames.
    bool consumeActivity();

    void setFps(double fps, double frameMs) {
        fps_ = fps;
        frameMs_ = frameMs;
    }
    bool dark() const { return dark_; }
    void setTheme(bool dark);
    // Rendering preferences owned by the Gui (View menu).
    bool lineweights() const { return lineweights_; }

    // Command-line widget callback (public so the ImGui callback trampoline can reach it).
    int inputCallback(ImGuiInputTextCallbackData* data);
    // Rebuilds fonts and style after the window moved to a monitor with another DPI scale.
    void setDpiScale(float scale);
    float dpiScale() const { return dpiScale_; }
    // Shows a registered panel (Tools > MCP Settings...).
    void openPanel(const std::string& id) { panelOpen_[id] = true; }

private:
    enum class Pending { None, New, Open, OpenRecent, Exit };
    enum class BrowserPurpose { Open, SaveAs };

    void shortcuts();
    void menuBar();
    void toolStrip();
    void statusBar();
    void dockLayout(ImGuiID dockspace);
    void commandWindow();
    void propertiesWindow();
    void panels();
    void dialogs();
    void viewportInput(const ViewportRect& rect);
    void registeredMenu(const std::string& top);
    void registeredSubmenu(const std::string& prefix, std::vector<const ui::MenuItemInfo*>& items);
    void runAction(const std::string& action);

    void doNew();
    void doOpen();
    void doOpenPath(const std::string& path);
    void doSave();
    void doSaveAs();
    void afterSavePrompt(bool saved);
    void requestAction(Pending what, std::string path = {});
    void performPending();
    void* nativeHandle() const;

    ui::AppCore& core_;
    ui::UiRegistry& registry_;
    GLFWwindow* window_;
    FileBrowser browser_;

    bool dark_ = true;
    bool lineweights_ = true;
    bool exitRequested_ = false;
    bool showAbout_ = false;
    bool showNotices_ = false;
    bool showCommandLine_ = true;
    bool showProperties_ = true;
    bool layoutBuilt_ = false;
    std::map<std::string, bool> panelOpen_{};
    float dpiScale_ = 1.0f;
    std::optional<ui::Recovery> recovery_{};
    bool openRecoveryPrompt_ = false;
    BrowserPurpose browserPurpose_ = BrowserPurpose::Open;
    std::string errorText_{};

    // Command line.
    std::string input_{};
    std::vector<std::string> inputHistory_{};
    int historyPos_ = -1;
    bool refocusInput_ = false;
    bool spaceAsEnter_ = false;
    std::string typedWhileUnfocused_{};
    std::size_t lastHistorySize_ = 0;
    ImGuiID inputId_ = 0;

    // Save prompt.
    Pending pending_ = Pending::None;
    std::string pendingPath_{};
    bool openSavePrompt_ = false;
    bool saveThenPending_ = false;
    std::string noticesText_{};
    bool noticesLoaded_ = false;
    std::string lastError_{};

    // Viewport interaction.
    bool leftDown_ = false;
    bool doubleClick_ = false;
    bool dragging_ = false;
    Vec2 pressPos_{};
    bool panning_ = false;
    double pendingNotches_ = 0;
    Vec2 zoomAnchor_{};
    int activityFrames_ = 0;
    bool anyActivity_ = false;
    ViewportRect lastRect_{};

    double fps_ = 0;
    double frameMs_ = 0;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
};

}  // namespace tbcad::app
