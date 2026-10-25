#pragma once

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "file_browser.h"
#include "settings_dialog.h"
#include "tbcad/core/color.h"
#include "tbcad/platform/external_link.h"
#include "tbcad/textcmd/format_bar.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/command_match.h"
#include "tbcad/ui/input_history.h"
#include "tbcad/ui/radial_wheel.h"
#include "tbcad/ui/theme.h"
#include "tbcad/ui/ui_registry.h"
#include "tbcad/ui/workspace.h"
#include "wheel_menu.h"

struct GLFWwindow;
struct ImFont;

namespace tbcad::app {

// Installs the platform hook that places the input method's candidate window at the text cursor.
void installImeHook(GLFWwindow* window);

// (Re)builds the UI font atlas: Latin (Lato) merged with Thai (Sarabun) at `scale`. Needs the
// OpenGL backend to be initialised; recreates the font texture.
void buildFonts(float scale);
// Monospace font (Liberation Mono, Thai merged from Sarabun) for the command log and the status
// bar coordinates; nullptr when the font file is missing (use the normal font then).
ImFont* monoFont();
// Dear ImGui colours and metrics from the theme tokens; `scale` is the DPI scale for sizes.
void applyTheme(const ui::Theme& theme, float scale);
// True when Windows is set to dark app mode (always true elsewhere).
bool systemPrefersDark();
// Where bundled assets live: next to the executable, then ./assets, then the source tree.
std::string findAsset(const std::string& relative);
// A drawing from the samples folder (next to the executable, ./samples, or the source tree), or
// empty.
std::string findSample(const std::string& name);
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
    // The viewport rectangle of the last frame (window points).
    const ViewportRect& lastViewportRect() const { return lastRect_; }
    // Shows the rendered drawing in the viewport rectangle; call after frame(), before Render().
    void showViewport(unsigned texture, const ViewportRect& rect);
    // Model tiled viewports (VPORTS): the tiles other than the active one, with their rectangles
    // (window points), and drawing one of them; call after frame(), before Render().
    const std::vector<std::pair<std::size_t, ViewportRect>>& inactiveTiles() const {
        return inactiveTiles_;
    }
    void showTile(unsigned texture, const ViewportRect& rect);

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
    bool lineweights() const;

    // Files dropped on the window; opened at the start of the next frame.
    void queueDroppedFile(std::string path);
    void pasteClipboardPicture();

    // Command-line widget callback (public so the ImGui callback trampoline can reach it).
    int inputCallback(ImGuiInputTextCallbackData* data);
    // Rebuilds fonts and style after the window moved to a monitor with another DPI scale.
    void setDpiScale(float scale);
    float dpiScale() const { return dpiScale_; }
    // Shows a registered panel (Tools > MCP Settings...).
    void openPanel(const std::string& id) { panelOpen_[id] = true; }
    // Turns the picture of a recent drawing (a PNG file) into a texture for the start page, or 0
    // when there is none. Set by the App; without it the cards show the file type instead.
    void setThumbnailLoader(std::function<unsigned(const std::filesystem::path&)> loader) {
        thumbnailLoader_ = std::move(loader);
    }

private:
    // CloseAll closes every drawing except the one `keepDoc_` names (0 = none): CLOSEALL and the
    // tab menu's "Close Other Drawings".
    enum class Pending { None, Close, Exit, CloseAll };
    std::uint64_t keepDoc_ = 0;
    bool keepDocSet_ = false;
    void closeAllDrawings(bool keepActive);
    bool askCombined(bool skipActive);
    void saveAllDrawings();
    // QVDRAWING / QVLAYOUT: cards of the open drawings (or of this drawing's layouts) to switch to.
    void quickView();
    int quickView_ = 0;    // 0 closed, 1 drawings, 2 layouts of the active drawing
    int tabCloseAll_ = 0;  // from the tab menu: 1 close all, 2 close the others
    enum class BrowserPurpose { Open, SaveAs, ImportDxf, ExportDxf, LayoutTemplate };

    void shortcuts();
    void menuBar();
    void toolStrip();
    void readOnlyBanner();
    void blockEditBar();
    void contextualBar();
    void xrefNotice();
    bool xrefListOpen_ = false;
    int statusRows_ = 1;               // rows the status bar needed last frame
    std::string dismissedReadOnly_{};  // read-only notice text the user closed
    void docTabs();
    // Hover popup with a line picture of a drawing or layout (QVDRAWING / QVLAYOUT).
    void tabPreviewTip(const std::string& title, const ui::TabPreview& preview);
    void startPage(const ViewportRect& rect);
    std::function<unsigned(const std::filesystem::path&)> thumbnailLoader_;
    bool ucsIconShown() const;
    void ucsIcon(const ViewportRect& rect);
    void statusBar();
    void snapModesPopup();
    void draftingQuickMenu(const std::string& id);
    float layoutTabsHeight() const;
    void layoutTabs(const ViewportRect& rect);
    void dockLayout(ImGuiID dockspace);
    void dockEdgeDrop();
    void keepOnScreen();
    // Workspaces (saved window layouts). Switching applies the panels at once and the docking
    // layout through Dear ImGui's ini data.
    void applyWorkspace(const ui::Workspace& ws);
    void switchWorkspace(const std::string& name);
    void saveWorkspace(const std::string& name);
    void workspaceMenu();
    void workspaceDialog();
    // DRAWINGRECOVERY: autosaves and .bak files to open.
    void recoveryManager();
    void commandWindow();
    void updateSuggestions();
    void drawSuggestions(ImVec2 inputPos, float width);
    bool optionChips();
    void cursorHud(const ViewportRect& rect);
    void gripMenu(const ViewportRect& rect);
    void repairAfterOpen();
    bool openLinkPrompt_ = false;  // HYPERLINK Open: ask before handing the target to the system
    platform::LinkCheck pendingLink_{};
    bool recoverAfterOpen_ = false;  // RECOVER <file>: repair the drawing once it is open
    void logWindow();
    std::string actionLabel(const std::string& action) const;
    void quickProperties(const ViewportRect& rect);
    void textEditor(const ViewportRect& rect);
    void panels();
    void fillPanelContext(ui::PanelContext& pc);
    void dialogs();
    void viewportInput(const ViewportRect& rect);
    ViewportRect tiledViewports(const ViewportRect& area);
    void navigationBar(const ViewportRect& rect);
    void viewportMenu();
    void cycleListPopup();
    void snapOverrideItems();
    void dropIntoViewport(const ViewportRect& rect);
    void registeredMenu(const std::string& top);
    void registeredSubmenu(const std::string& prefix, std::vector<const ui::MenuItemInfo*>& items);
    void runAction(const std::string& action);

    void doNew();
    void doOpen();
    void bringToFront();
    std::filesystem::path dialogFolder() const;
    void rememberFolder(const std::filesystem::path& picked);
    void doOpenPath(const std::string& path);
    void openPending();
    void closeTopModalOnEscape();
    void history(bool redo);
    void historyPending();
    void doImportDxf();
    void doLayoutFromTemplate();  // LAYOUT Template with a file chosen in a dialog
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
    bool exitRequested_ = false;
    bool showAbout_ = false;
    bool showNotices_ = false;
    bool showReport_ = false;
    bool showCommandList_ = false;
    std::string commandFilter_{};
    bool showCommandLine_ = true;
    bool layoutBuilt_ = false;
    ui::WorkspaceStore workspaces_;
    std::string workspaceName_{};  // the one last switched to or saved
    bool dockFallbackTried_ = false;
    std::string focusPanel_{};  // window title to bring to the front of its tab group
    int frontLayersIn_ = 0;     // frames until the Layers tab is brought to the front
    bool rebuildDock_ = false;  // rebuild the default docking layout next frame
    bool openWorkspaceSave_ = false;
    std::string workspaceNameInput_{};
    std::string workspaceMessage_{};
    std::map<std::string, bool> panelOpen_{};
    float dpiScale_ = 1.0f;
    std::optional<ui::Recovery> recovery_{};
    std::size_t recoveryCount_ = 0;
    std::vector<ui::Recovery> recoveries_{};  // the list in the start-up recovery prompt
    std::string pendingOpen_{};  // big file waiting for the "Opening ..." message to be shown
    bool pendingOpenShown_ = false;
    int pendingHistory_ = 0;  // 1 undo, 2 redo: waiting for its hint to be drawn
    bool pendingHistoryShown_ = false;
    std::string pendingHistoryLabel_{};
    std::string crashReport_{};  // newest unacknowledged crash report file, if any
    bool openRecoveryPrompt_ = false;
    bool recoveryManagerOpen_ = false;
    BrowserPurpose browserPurpose_ = BrowserPurpose::Open;
    std::string errorText_{};
    std::vector<std::string> droppedFiles_{};

    // Command line.
    std::string input_{};
    ui::InputHistory inputHistory_{};
    bool refocusInput_ = false;
    // The text COPYCLIP put on the system clipboard (the marker or the copied text); it means "our
    // objects" for Ctrl+V.
    std::optional<std::string> ownClipText_{};
    bool isOwnClipText(const char* text) const {
        return std::string_view(text).rfind("[TBCAD objects", 0) == 0 ||
               (ownClipText_ && *ownClipText_ == text);
    }
    bool textEditorActive_ = false;  // a text prompt is edited in a box in the drawing
    int textEditorFocus_ = 0;
    std::string textEditorBuf_{};
    std::string textEditorSent_{};
    textcmd::FormatBar formatBar_{};  // the MTEXT editor's formatting bar
    bool spaceAsEnter_ = false;
    std::string typedWhileUnfocused_{};
    std::size_t lastHistorySize_ = 0;
    float commandFooterH_ = 0;  // measured height of the chips and input line
    ImGuiID inputId_ = 0;
    // Autocomplete / command palette.
    std::vector<ui::CommandMatch> suggestions_{};
    int suggestSel_ = 0;
    bool suggestNavigated_ = false;  // an arrow key or click chose a row (Enter then runs it)
    bool suggestVisible_ = false;
    bool suggestDismissed_ = false;
    bool paletteOpen_ = false;
    std::string suggestFor_{};
    bool cursorHud_ = true;
    bool hudHovered_ = false;
    ui::HoverMenu gripMenu_{};  // the multifunctional grip menu being shown
    bool gripMenuOpen_ = false;
    bool gripMenuHovered_ = false;
    bool ctrlWasDown_ = false;
    int gripMenuSel_ = -1;  // row Ctrl moved to; -1 until it is used
    double gripMenuSince_ = 0;
    double gripMenuLast_ = 0;
    std::string gripMenuSeen_{};
    std::size_t activityStamp_ = 0;
    ImVec2 hudSize_{0, 0};
    ImVec2 hoverTipPos_{-1, -1};  // where the pointer rested, for the hover tooltip
    // Quick Properties next to the pointer: where it was when the selection last changed.
    ImVec2 quickPropsAt_{-1, -1};
    std::size_t quickPropsKey_ = 0;
    bool quickPropsPlaced_ = false;
    ImVec2 quickPropsSize_{300, 420};
    double hoverTipSince_ = 0;
    // A yes/no setting that also reads right when it was set as text (SETTING command, config).
    bool flag(std::string_view key, bool fallback) const;
    void hoverTooltip();
    // Short non-blocking notices at the bottom right of the viewport (saved, opened, ...).
    void toast(const std::string& text);
    void drawToasts(const ViewportRect& rect);
    // XREFNOTIFY 2: a balloon naming the external references whose file changed, to reload them.
    void drawXrefBalloon(const ViewportRect& rect);
    struct Toast {
        std::string text;
        double shownAt = 0;
    };
    std::vector<Toast> toasts_{};
    bool startPage_ = true;
    std::size_t shownActive_ = static_cast<std::size_t>(-1);
    ImVec2 hudAnchor_{-1e6f, -1e6f};

    // Command log (Ctrl+L) and clean-screen mode (F11).
    bool logExpanded_ = false;
    bool cleanScreen_ = false;
    std::vector<std::string> hiddenPanels_{};  // panels HIDEPALETTES closed, for SHOWPALETTES
    void windowCommand(const std::string& what);
    // Real-time ZOOM / PAN (Enter at ZOOM, PAN, the navigation bar): left drag zooms or pans until
    // Esc, Enter or a right click.
    enum class Realtime { Off, Zoom, Pan };
    Realtime realtime_ = Realtime::Off;
    bool realtimeDrag_ = false;
    // A click that only made another tiled viewport active.
    bool tileJustActivated_ = false;
    std::vector<std::pair<std::size_t, ViewportRect>> inactiveTiles_;
    std::size_t logShownSize_ = static_cast<std::size_t>(-1);  // forces a scroll on first show
    int logFollow_ = 0;

    // Save prompt.
    Pending pending_ = Pending::None;
    bool openSavePrompt_ = false;
    bool openCombinedPrompt_ = false;
    bool combinedAsked_ = false;  // the one-question prompt was shown for this close request
    bool discardAll_ = false;     // the user chose Discard all
    bool saveThenPending_ = false;
    // Save As to a .dxf asks for the DXF version first.
    std::filesystem::path pendingDxfPath_{};
    bool openDxfVersionPrompt_ = false;
    bool openChangedOnDiskPrompt_ = false;  // Save found the file changed by another program
    int dxfVersionChoice_ = 1;              // index into R12 / R2000 / R2007 as shown in the prompt
    std::vector<std::string> dxfVersionWarnings_{};
    void saveAsPath(const std::filesystem::path& path);
    std::string noticesText_{};
    bool noticesLoaded_ = false;
    bool showQuickStart_ = false;
    std::string helpShown_{};
    std::string helpCommand_{};  // F1 while a command runs: the command whose help is open
    std::string quickStartText_{};
    std::string lastError_{};

    // Viewport interaction.
    bool leftDown_ = false;
    bool pressTookPoint_ = false;  // the press already answered a point prompt
    bool doubleClick_ = false;
    bool dragging_ = false;
    std::vector<Vec2> lassoPath_{};  // Alt+drag freeform selection, screen pixels
    // Selection cycling list (SELECTIONCYCLING 2): the overlapping objects of the last click.
    std::vector<EntityId> cycleList_{};
    ui::PointerState cycleListAt_{};
    bool openCycleList_ = false;
    Vec2 pressPos_{};
    bool panning_ = false;
    bool rightDown_ = false;  // right button pressed in the drawing, wheel or menu pending
    ui::RightClick rightAction_ = ui::RightClick::Menu;  // what releasing it soon will do
    WheelMenu wheel_;
    double pendingNotches_ = 0;
    Vec2 zoomAnchor_{};
    int activityFrames_ = 0;
    bool anyActivity_ = false;
    ViewportRect lastRect_{};
    std::string layoutDialog_{};  // "new" / "rename", while its name popup is open
    std::string layoutTarget_{};
    std::string shownLayout_{};       // the layout on screen last frame ("" = Model)
    std::string previousLayout_{};    // the one shown before it
    float layoutScrollDelta_ = 0.0f;  // pending sideways scroll of the tab strip
    float layoutScrollMax_ = 0.0f;
    std::string layoutFollowed_{};  // the tab the strip last scrolled to
    std::string layoutNameBuf_{};

    double fps_ = 0;
    double frameMs_ = 0;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
};

}  // namespace tbcad::app
