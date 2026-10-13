#pragma once

#include <imgui.h>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "file_browser.h"
#include "settings_dialog.h"
#include "tbcad/core/color.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/command_match.h"
#include "tbcad/ui/theme.h"
#include "tbcad/ui/ui_registry.h"
#include "tbcad/ui/workspace.h"
#include "wheel_menu.h"

struct GLFWwindow;

namespace tbcad::app {

// Installs the platform hook that places the input method's candidate window at the text cursor.
void installImeHook(GLFWwindow* window);

// (Re)builds the UI font atlas: Latin (Lato) merged with Thai (Sarabun) at `scale`. Needs the
// OpenGL backend to be initialised; recreates the font texture.
void buildFonts(float scale);
// Dear ImGui colours and metrics from the theme tokens; `scale` is the DPI scale for sizes.
void applyTheme(const ui::Theme& theme, float scale);
// True when Windows is set to dark app mode (always true elsewhere).
bool systemPrefersDark();
// Where bundled assets live: next to the executable, then ./assets, then the source tree.
std::string findAsset(const std::string& relative);
// A file shipped next to the executable (THIRD_PARTY_NOTICES.txt), or empty.
std::string findBesideExecutable(const std::string& name);

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
    const ui::Theme& theme() const { return theme_; }
    ui::ThemeMode themeMode() const { return themeMode_; }
    void setThemeMode(ui::ThemeMode mode);
    // Drawing-area background: the theme's, or the `ui.viewportBackground` setting (#RRGGBB).
    Rgba viewportBackground() const;
    // Rendering preferences owned by the Gui (View menu).
    bool lineweights() const { return lineweights_; }

    // Files dropped on the window; opened at the start of the next frame.
    void queueDroppedFile(std::string path);

    // Command-line widget callback (public so the ImGui callback trampoline can reach it).
    int inputCallback(ImGuiInputTextCallbackData* data);
    // Rebuilds fonts and style after the window moved to a monitor with another DPI scale.
    void setDpiScale(float scale);
    float dpiScale() const { return dpiScale_; }
    // Shows a registered panel (Tools > MCP Settings...).
    void openPanel(const std::string& id) { panelOpen_[id] = true; }

private:
    enum class Pending { None, Close, Exit };
    enum class BrowserPurpose { Open, SaveAs, ImportDxf, ExportDxf };

    void shortcuts();
    void menuBar();
    void toolStrip();
    void readOnlyBanner();
    void docTabs();
    void startPage(const ViewportRect& rect);
    void statusBar();
    void dockLayout(ImGuiID dockspace);
    // Workspaces (saved window layouts). Switching applies the panels at once and the docking
    // layout through Dear ImGui's ini data.
    void applyWorkspace(const ui::Workspace& ws);
    void switchWorkspace(const std::string& name);
    void saveWorkspace(const std::string& name);
    void workspaceMenu();
    void workspaceDialog();
    void commandWindow();
    void updateSuggestions();
    void drawSuggestions(ImVec2 inputPos, float width);
    bool optionChips();
    void cursorHud(const ViewportRect& rect);
    void logWindow();
    std::string actionLabel(const std::string& action) const;
    void quickProperties(const ViewportRect& rect);
    void panels();
    void dialogs();
    void viewportInput(const ViewportRect& rect);
    void viewportMenu();
    void dropIntoViewport(const ViewportRect& rect);
    void registeredMenu(const std::string& top);
    void registeredSubmenu(const std::string& prefix, std::vector<const ui::MenuItemInfo*>& items);
    void runAction(const std::string& action);

    void doNew();
    void doOpen();
    void doOpenPath(const std::string& path);
    void doImportDxf();
    void doExportDxf();
    void dxfCommandWithPath(const char* command, const std::filesystem::path& path);
    void doSave();
    void doSaveAs();
    void afterSavePrompt(bool saved);
    void requestAction(Pending what);
    // After the save prompt: `discard` is true for "Don't Save".
    void performPending(bool discard = false);
    void continuePending();
    void* nativeHandle() const;

    ui::AppCore& core_;
    ui::UiRegistry& registry_;
    GLFWwindow* window_;
    FileBrowser browser_;
    SettingsDialog settings_;

    // Dynamic input field being edited in the cursor HUD.
    void dynamicFields(const cmd::FilterFeedback& fb);
    int dynFocus_ = -1;
    bool dynFocusPending_ = false;
    std::string dynBuf_{};

    ui::ThemeMode themeMode_ = ui::ThemeMode::Dark;
    ui::Theme theme_ = ui::darkTheme();
    bool lineweights_ = true;
    bool exitRequested_ = false;
    bool showAbout_ = false;
    bool showNotices_ = false;
    bool showCommandLine_ = true;
    bool layoutBuilt_ = false;
    ui::WorkspaceStore workspaces_;
    std::string workspaceName_{};  // the one last switched to or saved
    bool rebuildDock_ = false;     // rebuild the default docking layout next frame
    bool openWorkspaceSave_ = false;
    std::string workspaceNameInput_{};
    std::string workspaceMessage_{};
    std::map<std::string, bool> panelOpen_{};
    float dpiScale_ = 1.0f;
    std::optional<ui::Recovery> recovery_{};
    std::size_t recoveryCount_ = 0;
    std::string crashReport_{};  // newest unacknowledged crash report file, if any
    bool openRecoveryPrompt_ = false;
    BrowserPurpose browserPurpose_ = BrowserPurpose::Open;
    std::string errorText_{};
    std::vector<std::string> droppedFiles_{};

    // Command line.
    std::string input_{};
    std::vector<std::string> inputHistory_{};
    int historyPos_ = -1;
    bool refocusInput_ = false;
    bool spaceAsEnter_ = false;
    std::string typedWhileUnfocused_{};
    std::size_t lastHistorySize_ = 0;
    ImGuiID inputId_ = 0;
    // Autocomplete / command palette.
    std::vector<ui::CommandMatch> suggestions_{};
    int suggestSel_ = 0;
    bool suggestVisible_ = false;
    bool suggestDismissed_ = false;
    bool paletteOpen_ = false;
    std::string suggestFor_{};
    bool cursorHud_ = true;
    bool hudHovered_ = false;
    bool startPage_ = true;
    std::size_t shownActive_ = static_cast<std::size_t>(-1);
    ImVec2 hudAnchor_{-1e6f, -1e6f};

    // Command log (Ctrl+L) and clean-screen mode (F11).
    bool logExpanded_ = false;
    bool cleanScreen_ = false;
    std::size_t logShownSize_ = static_cast<std::size_t>(-1);  // forces a scroll on first show
    int logFollow_ = 0;

    // Save prompt.
    Pending pending_ = Pending::None;
    bool openSavePrompt_ = false;
    bool saveThenPending_ = false;
    std::string noticesText_{};
    bool noticesLoaded_ = false;
    bool showQuickStart_ = false;
    std::string quickStartText_{};
    std::string lastError_{};

    // Viewport interaction.
    bool leftDown_ = false;
    bool doubleClick_ = false;
    bool dragging_ = false;
    std::vector<Vec2> lassoPath_{};  // Alt+drag freeform selection, screen pixels
    Vec2 pressPos_{};
    bool panning_ = false;
    bool rightDown_ = false;  // right button pressed in the drawing, wheel or menu pending
    WheelMenu wheel_;
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
