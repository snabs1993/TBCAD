#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/cmd/runner.h"
#include "tbcad/cmd/undo.h"
#include "tbcad/core/core.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/index/spatial_index.h"
#include "tbcad/platform/keybindings.h"
#include "tbcad/platform/paths.h"
#include "tbcad/platform/settings.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/image.h"
#include "tbcad/render/overlay.h"
#include "tbcad/render/scene.h"
#include "tbcad/ui/foreign_files.h"
#include "tbcad/ui/paper_view.h"
#include "tbcad/ui/scene_sync.h"
#include "tbcad/ui/theme.h"
#include "tbcad/ui/ui_registry.h"
#include "tbcad/underlay/image.h"

namespace tbcad::layout {
struct Viewport;
}

namespace tbcad::ui {

struct PointerState {
    Vec2 screen{};  // pixels in the viewport, origin top-left
    bool shift = false;
    bool ctrl = false;
};

// An autosave left behind by a session that did not end cleanly.
struct Recovery {
    std::filesystem::path autosaveFile{};
    std::string originalPath{};    // empty = the drawing had never been saved
    std::string savedAt{};         // when the backup was written, local time "2026-10-15 14:32"
    std::uintmax_t sizeBytes = 0;  // size of the autosave file
};

// Shown after a command put geometry outside the working area (non-blocking notice with "Zoom to"
// and "Undo" actions).
struct OutOfAreaNotice {
    std::vector<EntityId> ids{};
};

// Everything the TBCAD program does except draw a frame: it owns the document, undo stack,
// spatial index, scene, command runner, settings and key bindings, handles file operations,
// viewport navigation, picking and selection, and implements cmd::IHost. The window code (GLFW,
// Dear ImGui, OpenGL) only forwards events to it and draws what it exposes, so the whole
// application model runs headless in tests.
class AppCore final : public cmd::IHost {
public:
    // `registry` must already hold every command; AppCore adds ZOOM, PAN and DEVGEN. Settings,
    // key bindings and aliases are read from `paths`.
    AppCore(cmd::Registry& registry, platform::AppPaths paths);
    ~AppCore() override;
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // ---- startup / shutdown ----------------------------------------------------------------
    // Problems found while reading settings, keybindings and aliases (shown in the history).
    const std::vector<std::string>& startupWarnings() const { return startupWarnings_; }
    // Autosaves from a crashed session, if any.
    // Unsaved drawings found in the autosave folder (one entry per drawing), and the first of them.
    std::vector<Recovery> pendingRecoveries() const;
    std::optional<Recovery> pendingRecovery() const;
    Result<void> recover(const Recovery& r);
    // Opens every pending recovery (the first replaces the untouched drawing, the rest get tabs).
    Result<void> recoverAll();
    void discardRecovery();
    // ---- the Drawing Recovery Manager (DRAWINGRECOVERY) ----
    // Autosaves that are not of a drawing open now (left by a session that did not close
    // normally), and the .bak copies of the recent and open drawings (originalPath = the drawing).
    std::vector<Recovery> leftoverRecoveries() const;
    std::vector<Recovery> backupFiles() const;
    // Opens an autosave or backup in a new tab as an unsaved drawing, so saving asks for a name
    // and never replaces the original by accident. `removeAfter` deletes the autosave once it is
    // open (its drawing gets autosaves of its own).
    Result<void> openRecovered(const Recovery& r, bool removeAfter);
    // Writes settings; removes the autosave. Call on a normal exit.
    void shutdown();

    // ---- document ----------------------------------------------------------------------------
    Document& document();
    const Document& document() const;
    const index::SpatialIndex& spatialIndex() const;
    render::Scene& scene() { return scene_; }
    // ---- layouts (paper space) ---------------------------------------------------------------
    // The tab strip's model: "" is Model, otherwise a layout name. A layout is shown read-only
    // (the sheet, its objects and the viewports); editing it is done with the layout commands.
    const std::string& activeLayout() const { return activeLayout_; }
    bool inPaperSpace() const { return !activeLayout_.empty(); }
    // The viewport being edited from model space (entered by a double click or MSPACE): its
    // layout and id; empty and 0 when none.
    const std::string& editingLayout() const { return editingLayout_; }
    int editingViewport() const { return editingViewport_; }
    std::vector<std::string> layoutNames() const;
    // Empty or an unknown name goes back to Model. Returns false when the layout can't be shown.
    bool setActiveLayout(const std::string& name);
    // What the window draws: the drawing, or the active layout's sheet.
    render::Scene& renderScene() { return inPaperSpace() ? paper_.scene() : scene_; }
    cmd::UndoStack& undoStack();
    cmd::CommandRunner& runner();
    cmd::Registry& registry() { return registry_; }
    // Incremented whenever a different document is loaded; lets the window drop caches.
    std::uint64_t documentGeneration() const { return generation_; }

    // New and open add a tab. An untouched "Untitled" drawing is reused instead of keeping an
    // empty tab; a file that is already open is just activated.
    // Starts from the default template (setting "template.default": a template name, empty or
    // "Built-in" = the built-in drawing). A template that cannot be read falls back to the
    // built-in drawing and says so in the history.
    void newDocument();
    Result<void> openFile(const std::filesystem::path& path);

    // ---- open drawings (tabs) ----------------------------------------------------------------
    std::size_t documentCount() const;
    std::size_t activeDocument() const { return active_; }
    // Switches to drawing `index`: its view and selection come back. Refused while a command
    // runs. Returns whether `index` is active afterwards.
    bool activateDocument(std::size_t index);
    // Closes drawing `index` without asking (the window asks about unsaved changes first). Closing
    // the last drawing leaves one empty "Untitled".
    void closeDocument(std::size_t index);
    std::string documentTitle(std::size_t index) const;
    bool documentModified(std::size_t index) const;
    bool anyDocumentModified() const;
    // Stable while the drawing stays open, whatever happens to the other tabs.
    std::uint64_t documentId(std::size_t index) const;
    // Untitled, unmodified and empty: nothing the user would miss.
    bool isPristine() const;
    // Saves to the current file; fails with NotFound-style error text when there is none yet
    // (the window then asks for a name and calls saveAs).
    Result<void> save();
    Result<void> saveAs(const std::filesystem::path& path);
    bool hasFile() const;
    const std::filesystem::path& file() const;
    // Foreign formats. `extension` is lower case with the dot (".dwg"). A read-only format
    // opens as a copy: Save always asks for a new name and never writes the original.
    void setForeignReader(std::string extension, ForeignReadFn read, bool readOnly);
    bool canOpenExtension(std::string_view lowerExtension) const;
    // Opens a foreign file on a worker thread so the window keeps drawing (and can show how long
    // it has been). False when the file is not a foreign format or one open is already running;
    // the caller then uses openFile. pollOpen() finishes it on the main thread, once per frame:
    // it returns the outcome when the open has just ended, nothing while it is still running.
    bool openFileAsync(const std::filesystem::path& path);
    std::optional<Result<void>> pollOpen();
    struct OpeningInfo {
        std::filesystem::path path;
        double seconds = 0;
    };
    std::optional<OpeningInfo> opening() const;
    // True while the drawing came from a read-only format and has not been saved as .tbcad.
    bool readOnlyOrigin() const;
    // True for the frame before a changed external reference is reloaded, so the window can show
    // a "Reloading external references" card before the command blocks.
    bool xrefReloading() const { return xrefReloadWait_ > 0; }
    // The window shows a failed open in its own message box; it turns the command-line echo off so
    // the message is not shown twice.
    void setEchoOpenFailures(bool on) { echoOpenFailures_ = on; }
    // One-line banner for a read-only drawing; empty otherwise.
    std::string readOnlyBanner() const;
    // External references whose file cannot be found, scanned when a drawing is opened or
    // activated. The window shows a notice until it is dismissed (a new one shows it again).
    struct MissingXref {
        std::string name;
        std::string path;  // as stored in the drawing
    };
    const std::vector<MissingXref>& missingXrefs() const;
    bool xrefNoticeVisible() const;
    void dismissXrefNotice();
    bool modified() const;
    // "Untitled" or the file name.
    std::string displayName() const;
    // "drawing.tbcad* - TBCAD"
    std::string windowTitle() const;
    std::vector<std::string> recentFiles() const { return settings_.recentFiles(); }
    // The picture of a drawing that the start page shows next to its name (the file may not
    // exist): <data folder>/thumbnails/<hash of the path>.png.
    std::filesystem::path thumbnailFile(const std::string& drawingPath) const;
    // The drawing the window should take a picture of now: the one just opened or saved, once it
    // is the active drawing and its geometry is built. Empty when there is none.
    std::string pendingThumbnail() const;
    void thumbnailTaken() { thumbnailFor_.clear(); }

    // ---- undo / redo -------------------------------------------------------------------------
    bool undo();
    bool redo();
    // True when the next undo (redo) is big enough to block the window for a noticeable time, so
    // the caller can show a hint first. The limit is the setting "undo.busyBytes" (default 16 MB).
    bool historyStepHeavy(bool redo) const;
    std::string undoLabel() const;  // "Undo Line" ; empty when nothing to undo or a command runs
    std::string redoLabel() const;

    // ---- command line ------------------------------------------------------------------------
    // Submits a line typed in the command line. When idle the first word names a command and the
    // remaining words answer its prompts ("DEVGEN LINES 1000000"); empty text repeats the last
    // command (or, in a prompt, takes the default).
    void submitText(std::string_view text);
    // A script line as --script and the gate take it: each part between semicolons is submitted
    // in turn, an empty part being Enter ("ERASE;ALL;" erases everything).
    void submitScript(std::string_view script);
    // The same for a line a panel sends: the command line shows names instead of the internal
    // "#id" references the panels use, and answers to prompts are not echoed a second time.
    void submitPanelText(std::string_view text);
    void pressEscape();
    const std::vector<std::string>& history() const { return history_; }
    void print(std::string_view message) override;
    // Prompt line for the command line widget, e.g. "Specify next point or [Close/Undo]: ".
    std::string promptText() const;
    // Text a command put in the `clipboard.text` setting for the system clipboard (COPYRESULT).
    // Returns it once.
    std::optional<std::string> takeClipboardText();
    // The pending prompt, for option chips and the cursor HUD. Null when idle.
    const cmd::PromptInfo* promptInfo() const { return prompt_ ? &*prompt_ : nullptr; }
    // Command names and aliases beginning with `prefix`, for completion.
    std::vector<std::string> complete(std::string_view prefix) const;

    // ---- viewport ----------------------------------------------------------------------------
    const render::Camera2D& camera() const { return camera_; }
    void setViewportSize(int w, int h);
    // Where the canvas starts on the window (pixels). A panel docked at the left or top moves it;
    // the drawing then stays where it is on the screen instead of sliding with the canvas.
    void setViewportOrigin(double x, double y);
    void setCamera(const render::Camera2D& cam);
    // `notches` mouse-wheel notches (fractions allowed); factor 1.2 per notch.
    void zoomAtScreen(Vec2 screen, double notches);
    void panPixels(double dx, double dy);
    void zoomExtents();
    BBox2 drawingExtents() const;
    BBox2 computeDrawingExtents() const;
    // Where the time of the last file open went: step name and milliseconds (gate-file prints it).
    const std::vector<std::pair<std::string, double>>& openTimings() const { return openTimings_; }
    void zoomToBox(const BBox2& box);
    void zoomToEntities(const std::vector<EntityId>& ids);

    // ---- tiled model viewports (VPORTS) ------------------------------------------------------
    // The drawing area of model space can be split into tiles, each with its own view; commands
    // and the pointer work in the active one (camera()). Rectangles are fractions of the drawing
    // area, y downwards.
    struct ViewTile {
        double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        Vec2 center{};
        double pixelsPerMm = 1;
    };
    // The tiles while model space is shown (one = not tiled); inactive tiles keep their view here.
    const std::vector<ViewTile>& tiles() const { return tiles_; }
    std::size_t activeTile() const { return activeTile_; }
    bool tiled() const { return tiles_.size() > 1 && !inPaperSpace() && editingLayout_.empty(); }
    // "1", "2V", "2H", "3R", "3L", "3A", "3B", "3V", "3H" or "4"; every tile starts with the
    // current view. False for an unknown code.
    bool setTileLayout(std::string_view code);
    const std::string& tileLayout() const { return tileLayout_; }
    // Makes tile `i` the active one; (x, y, w, h) is where it is on the window, in pixels.
    void activateTile(std::size_t i, double x, double y, int w, int h);
    // The camera of tile `i` shown at w x h pixels.
    render::Camera2D tileCamera(std::size_t i, int w, int h) const;
    // Tessellate for the current zoom when it changed enough; call once per frame.
    void updateFrame(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // True while the geometry of a big edit is still being built (frames should keep coming).
    bool sceneBuilding() const;
    // True while new objects are flashing (the window keeps drawing frames until it ends).
    bool flashing() const { return !flashIds_.empty(); }
    // Snap / tracking feedback of the point prompt in progress (tooltip text for the window).
    const cmd::FilterFeedback& feedback() const { return feedback_; }
    // Pixel position of the status-bar coordinates.
    Vec2 cursorWorld() const { return cursorWorld_; }

    // ---- pointer events (from the window) ----------------------------------------------------
    void pointerMoved(const PointerState& p);
    void pointerLeft();
    // A left click that did not turn into a drag.
    // Pressing the button on a grip of the selection starts its edit at once (so a press-drag-
    // release moves it). True when the press was taken; call `click` for everything else.
    bool pressOnGrip(const PointerState& p);
    // The menu a module offers for what lies under the cursor (a multifunctional grip), if any.
    bool hoverMenu(HoverMenu& out);
    void click(const PointerState& p);
    // A double click while no command runs: a text object under the pointer is selected and
    // edited with TEXTEDIT; anything else behaves like a single click.
    void doubleClick(const PointerState& p);
    // A finished rectangle drag from `a` to `b` (screen pixels). Left to right selects what is
    // completely inside (window), right to left what it touches (crossing). Shift removes.
    void dragSelect(Vec2 a, Vec2 b, const PointerState& p);
    // Live selection rectangle shown in the overlay while dragging; nullopt = none.
    // How many objects the selection box being dragged would pick right now (none: no drag).
    std::optional<std::size_t> dragSelectCount();
    // SELECTIONPREVIEW 1 (default): the objects the box would pick are highlighted while dragging.
    void setSelectionDrag(std::optional<std::pair<Vec2, Vec2>> corners);
    // Freeform selection: the screen-space path shown while dragging (empty = none) and the
    // selection made when it is released.
    void setLassoPath(std::vector<Vec2> screenPath) { lasso_ = std::move(screenPath); }
    void lassoSelect(const std::vector<Vec2>& screenPath, const PointerState& p);
    // Click that cycles through the overlapping objects under the cursor (Alt+click): each click
    // at the same spot picks the next one down.
    void cycleClick(const PointerState& p);
    // The selection cycling list (SELECTIONCYCLING 2): the objects a click here could pick,
    // nearest first; empty when a click would not pick (a non-pick prompt is waiting).
    std::vector<EntityId> objectsAt(const PointerState& p);
    // Picks one object of that list, as a click on it alone would.
    void pickFromList(EntityId id, const PointerState& p);
    // Outlines an object of the list while the pointer is over its row.
    void highlightCandidate(EntityId id);

    // ---- selection ---------------------------------------------------------------------------
    const std::vector<EntityId>& selection() const override { return selection_; }
    void setSelection(std::vector<EntityId> ids) override;
    void clearSelection() { setSelection({}); }
    // Changes whenever the selection changes (never 0), for panels that keep what they found.
    std::uint64_t selectionRevision() const { return selectionRevision_; }
    void selectAll();
    // Aperture of the pick box in mm at the current zoom.
    double apertureMm() const;

    // Colours of the overlay (crosshair, selection boxes, snap marker) come from the UI theme.
    void setTheme(const Theme& theme) { theme_ = theme; }
    const Theme& theme() const { return theme_; }

    // The object under the pickbox (null when none) and a one-line description of it for the
    // hover tooltip: "Line, layer Walls, length 1250.00 mm".
    EntityId hoveredEntity() const { return hoverId_; }
    std::string describeEntity(EntityId id) const;

    // ---- overlay -----------------------------------------------------------------------------
    // Crosshair, pick box, command preview, snap marker, selection rectangle and the overlay
    // drawers modules registered. Cleared first.
    void buildOverlay(render::Overlay& overlay, const UiRegistry* ui = nullptr);
    // What lies under the drawing (the grid): the modules' underlay drawers. Cleared first; the
    // window draws it before the scene.
    void buildUnderlay(render::Overlay& underlay, const UiRegistry* ui = nullptr);

    // ---- notices -----------------------------------------------------------------------------
    const std::optional<OutOfAreaNotice>& outOfAreaNotice() const { return outOfArea_; }
    void dismissOutOfAreaNotice() { outOfArea_.reset(); }
    // The notice's "Zoom to" and "Undo" actions.
    void outOfAreaZoomTo();
    void outOfAreaUndo();

    // ---- settings / key bindings -------------------------------------------------------------
    platform::Settings& settings() { return settings_; }
    platform::Keybindings& keybindings() { return keybindings_; }
    const platform::AppPaths& paths() const { return paths_; }
    // Declares the default chords of the modules' menu items, then re-reads keybindings.json so
    // the user's choices win. Also installs the registry's point filters and line parser into the
    // command runner (now and for every document opened later). Call once after registerAllUi;
    // the registry must outlive this object.
    void applyUiKeybindings(const UiRegistry& ui);
    // Adds the SNAP / GRID / ORTHO / POLAR / OSNAP toggles (stored in settings, readable by
    // drafting aids through IHost::setting("drafting.snap") ...).
    void registerBuiltinUi(UiRegistry& ui);
    // The seconds-based autosave check; the window calls it every frame.
    void tickAutosave(std::chrono::steady_clock::time_point now);
    // Once every couple of seconds: looks for external references whose file changed on disk.
    // XREFNOTIFY ("xref.notify") says what follows: 0 nothing, 1 a marker in the status bar,
    // 2 (default) the marker and a balloon naming the new changes, 3 reload them by itself
    // (XRELOAD) once no command is running. The window calls it every frame.
    void tickXrefs(std::chrono::steady_clock::time_point now);
    // Names of the external references of the active drawing whose file changed, as of the last
    // check; empty with XREFNOTIFY 0.
    std::vector<std::string> changedXrefs() const;
    // Names for the balloon (XREFNOTIFY 2): references that changed since the balloon was last
    // dismissed. Each change of a file is announced once.
    std::vector<std::string> xrefChangeNotice() const;
    void dismissXrefChangeNotice();
    // Reloads the named references (all changed ones when `names` is empty) and clears the balloon.
    void reloadXrefs(const std::vector<std::string>& names);
    // The picture underlays (IMAGEATTACH) that are visible, to draw before the scene. Pictures are
    // read from disk once and kept while the drawing shows them.
    std::vector<render::ImageDraw> underlayImages();
    // Writes the autosave of every open drawing that has unsaved changes and waits until it is on
    // disk. True if any was written. (tickAutosave does the same without waiting.)
    bool autosaveNow();
    // Waits until the autosaves started by tickAutosave are on disk (tests, and before quitting).
    void waitForAutosave();

    // ---- cmd::IHost --------------------------------------------------------------------------
    std::optional<std::string> setting(std::string_view key) const override;
    double ucsAngle() const override;
    bool queueScript(std::vector<std::string> lines, int repeat) override;
    bool repeatCommand(const std::string& name) override;
    void setSetting(std::string_view key, std::string value) override;
    std::filesystem::path settingsFolder() const override { return paths_.configDir; }
    // Key binding profiles (keybindings.json format). Both return an error text, empty on success.
    std::string exportKeybindings(const std::filesystem::path& file);
    std::string importKeybindings(const std::filesystem::path& file);
    // ---- templates ---------------------------------------------------------------------------
    std::filesystem::path templateFolder() const { return paths_.configDir / "templates"; }
    std::vector<std::string> templateNames() const override;
    std::string saveAsTemplate(std::string_view name) override;
    std::string newFromTemplate(std::string_view name) override;
    std::string documentFile() const override { return platform::pathToUtf8(file()); }
    void textEdited(const std::string& text) { runner().textEdited(text); }
    void openPanel(std::string_view panelId) override { panelRequests_.emplace_back(panelId); }
    void closePanel(std::string_view panelId) override { panelCloses_.emplace_back(panelId); }
    bool dialogsAvailable() const override { return dialogsAvailable_; }
    void setDialogsAvailable(bool on) { dialogsAvailable_ = on; }
    std::vector<std::string> takePanelCloses() { return std::exchange(panelCloses_, {}); }
    // Panels asked for since the last call (the window opens them).
    std::vector<std::string> takePanelRequests() { return std::exchange(panelRequests_, {}); }
    cmd::ViewState view() const override;
    std::optional<std::vector<EntityId>> entitiesInWindow(const BBox2& window) const override;
    std::optional<std::vector<EntityId>> selectShape(SelectShape shape,
                                                     const std::vector<Vec2>& pts) override;
    BlockId drawingOwner() const override;
    void setView(const cmd::ViewState& v) override;
    BBox2 viewExtents() const override;
    void setPreview(const DisplayList* list) override;
    void requestUi(std::string_view what) override {
        if (what.rfind("space:", 0) == 0)
            pendingSpace_ = what.substr(6);  // MSPACE / PSPACE, applied when the command ends
        else if (what.rfind("zoomxp:", 0) == 0)
            pendingZoomXp_ = std::atof(std::string(what.substr(7)).c_str());
        else if (what.rfind("vports:", 0) == 0)
            pendingVports_.emplace_back(what.substr(7));  // applied when the command ends
        else
            uiRequests_.emplace_back(what);
    }
    // Requests commands made since the last call ("settings"), oldest first.
    std::vector<std::string> takeUiRequests() { return std::exchange(uiRequests_, {}); }
    void promptChanged(const cmd::PromptInfo* prompt) override;
    void feedbackChanged(const cmd::FilterFeedback& feedback) override;

private:
    struct Session;
    struct DocSlot;
    class AppObserver;

    void wantThumbnail(const std::filesystem::path& path);
    std::unique_ptr<Session> makeSession(std::unique_ptr<Document> doc, std::filesystem::path file);
    Result<void> openDxf(const std::filesystem::path& path);
    void storeCurrentUcs();
    Result<void> saveDxf(const std::filesystem::path& path);
    void clearTransientState();
    // Background autosave: starts one (false if nothing to save or one is running), collects a
    // finished one (or waits for it), starts one per drawing without waiting.
    bool startAutosave(DocSlot& slot);
    void reapAutosave(DocSlot& slot, bool wait);
    void autosaveInBackground();
    void leavePaper();  // back to Model, restoring the model view
    void scanMissingXrefs();
    // Replaces the active drawing.
    void installSession(std::unique_ptr<Document> doc, std::filesystem::path file);
    // Adds a drawing and makes it the active one (or replaces an untouched Untitled one).
    void addSession(std::unique_ptr<Document> doc, std::filesystem::path file);
    // Makes slots_[index] the active one: view, selection and scene.
    void enterSlot(std::size_t index, bool fitView);
    std::optional<std::size_t> slotOfFile(const std::filesystem::path& path) const;
    void resetSessionState();
    void removeAutosaveFiles();
    void removeAutosaveOf(std::uint64_t slotId);
    cmd::RawInput makeRaw(const PointerState& p) const;
    void updateHover(const PointerState& p);
    EntityId hoverId_{};
    Vec2 viewportOrigin_{};
    bool haveOrigin_ = false;
    bool trimWhenSettled_ = false;  // a big build ran: trim the scene's spare memory when done
    int fitPending_ = 0;            // re-fits left after a drawing opened, see setViewportSize
    std::chrono::steady_clock::time_point fitUntil_{};  // re-fits only count until this time
    void applyPick(const std::vector<EntityId>& ids, bool remove);
    // A click or window on the idle drawing, honouring PICKADD (0: replaces; Shift toggles).
    void applyUserPick(const std::vector<EntityId>& ids, bool shift);
    void pruneSelection();
    void notifySessionHooks();

    struct ForeignReader {
        ForeignReadFn read{};
        bool readOnly = true;
    };
    std::map<std::string, ForeignReader, std::less<>> foreignReaders_{};
    struct PendingOpen;
    Result<void> finishForeignOpen(const std::filesystem::path& path, const std::string& ext,
                                   bool readOnly, Result<ForeignOpenResult> res);
    std::shared_ptr<PendingOpen> pendingOpen_{};
    bool echoOpenFailures_ = true;
    void printOpenFailure(const std::string& text);
    cmd::Registry& registry_;
    cmd::PointFilterChain filterChain_{};
    cmd::CommandLineParser* lineParser_ = nullptr;
    PanelContext makePanelContext();
    std::vector<std::string> uiRequests_{};
    const UiRegistry* ui_ = nullptr;
    // A copy of the default template; `problem` gets a sentence when the chosen one is unusable
    // and the built-in drawing was used instead.
    std::unique_ptr<Document> defaultDocument(std::string* problem = nullptr);
    platform::AppPaths paths_;
    std::string thumbnailFor_;  // pendingThumbnail()
    std::uint64_t thumbnailDoc_ = 0;
    std::vector<std::string> panelRequests_{};
    std::uint64_t selectionRevision_ = 1;
    std::vector<std::string> panelCloses_{};
    bool dialogsAvailable_ = false;
    std::map<std::string, std::string> transient_{};  // "tmp.*" settings: not saved
    platform::Settings settings_;
    platform::Keybindings keybindings_;
    std::vector<std::string> startupWarnings_{};

    render::Scene scene_;
    std::vector<DocSlot> slots_;
    std::size_t active_ = 0;
    std::uint64_t nextDocId_ = 1;
    Session* session_ = nullptr;  // slots_[active_]
    std::uint64_t generation_ = 0;
    // drawingExtents() walks every object; open asks for it twice for the same document state.
    mutable struct {
        const void* session = nullptr;
        std::uint64_t revision = 0;
        BBox2 box{};
    } extentsCache_{};
    std::vector<std::pair<std::string, double>> openTimings_;
    std::chrono::steady_clock::time_point openLapAt_{};
    void openLap(const std::string& name);
    // While a file opens: the start of the current step inside a lap, so the lap's parts show up
    // in openTimings() too (indented). Empty at other times, when openStep records nothing.
    std::optional<std::chrono::steady_clock::time_point> openStepAt_{};
    void openStep(const char* name);

    render::Camera2D camera_{};
    std::vector<ViewTile> tiles_{ViewTile{}};
    std::size_t activeTile_ = 0;
    std::string tileLayout_ = "1";
    std::vector<std::string> pendingVports_{};
    void applyPendingVports();
    // The view the mouse wheel or a drag started from goes into ZOOM Previous once per gesture.
    void rememberGestureStart();
    std::chrono::steady_clock::time_point lastGesture_{};
    std::string activeLayout_{};
    PaperView paper_{};
    // The objects drawn on the sheet, for picking and snapping while a layout is on screen.
    index::SpatialIndex paperIndex_{};
    void rebuildPaperIndex();
    render::Camera2D modelCamera_{};
    std::uint64_t paperRevision_ = 0;
    render::Camera2D lastTessellatedFor_{};
    // Stale objects out of view were left for later: rescan when the camera moves.
    bool staleWaiting_ = false;
    std::size_t retileCheckedAt_ = 0;  // entity count of the last fragmentation check
    void retileIfFragmented();
    // New objects glow for a moment so a line drawn over same-coloured lines can be found.
    void flashNew(std::vector<EntityId> ids);
    std::vector<EntityId> flashIds_{};
    std::chrono::steady_clock::time_point flashUntil_{};
    std::size_t shownPicked_ =
        0;  // objects picked at a Select objects prompt that are drawn selected
    Vec2 cursorWorld_{};
    bool cursorInside_ = false;
    Theme theme_ = darkTheme();
    PointerState lastPointer_{};

    std::vector<EntityId> selection_{};
    std::vector<Vec2> lasso_{};
    std::vector<EntityId> cycleStack_{};
    std::optional<EntityId> cycleCurrent_{};
    Vec2 cycleAt_{};
    // Outlines of the selected hatches, kept between frames (see buildOverlay).
    struct HatchOutlines {
        std::uint64_t key = ~0ull;
        std::vector<std::vector<Vec2>> loops{};
    } hatchOutlines_{};
    std::vector<std::string> history_{};
    std::optional<cmd::PromptInfo> prompt_{};
    cmd::FilterFeedback feedback_{};
    DisplayList preview_{};
    bool hasPreview_ = false;
    std::optional<std::pair<Vec2, Vec2>> drag_{};
    struct DragCount {
        Vec2 a{}, b{};
        std::uint64_t revision = ~0ull;
        std::size_t count = 0;
        std::vector<EntityId> ids{};
    } dragCount_{};
    std::vector<EntityId> dragPreview_{};
    std::optional<OutOfAreaNotice> outOfArea_{};
    bool inUndo_ = false;
    std::optional<std::string> clipboardOut_{};

    std::chrono::steady_clock::time_point lastAutosave_{};
    std::chrono::steady_clock::time_point settingsDirtyAt_{};
    bool settingsDirtySeen_ = false;
    bool autosaveClockStarted_ = false;
    bool quickJournalPending_ = false;
    std::chrono::steady_clock::time_point quickJournalAt_{};
    std::chrono::steady_clock::time_point lastXrefCheck_{};
    int xrefReloadWait_ = 0;
    bool viewStatePending_ = false;
    enum class EchoMode { Typed, Names, None };
    EchoMode echoMode_ = EchoMode::Typed;
    std::string friendlyEcho(const std::string& line) const;
    std::string pendingLayout_;  // layout to show when the running command ends
    void applyPendingLayout();
    std::string pendingSpace_;  // "model" or "paper"
    std::string lastLayout_;    // the sheet we last left for model space
    void applyPendingSpace();
    void enterViewport(const std::string& layoutName, const layout::Viewport& vp);
    double pendingZoomXp_ = 0;  // ZOOM nXP: the viewport scale as a fraction of paper space
    void hideViewportFrozenLayers();
    std::string vpHiddenKey_{};  // layout:viewport:revision the hidden layers were found for
    const void* vpHiddenSession_ = nullptr;
    std::string editingLayout_;  // the viewport being edited from model space, if any
    int editingViewport_ = 0;
    // The view when the viewport was entered: zooms and pans made since go back into the
    // viewport (its scale and centre) when the sheet is shown again, as in AutoCAD.
    double editEntryPixelsPerMm_ = 0;
    Vec2 editEntryCenter_{};
    void commitViewportView();
    void applyZoomXp();
    void followLockedViewport();
    std::uint64_t annoSyncRevision_ = 0;
    underlay::ImageCache imageCache_{};
};

}  // namespace tbcad::ui
