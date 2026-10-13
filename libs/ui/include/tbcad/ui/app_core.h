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
#include "tbcad/render/overlay.h"
#include "tbcad/render/scene.h"
#include "tbcad/ui/foreign_files.h"
#include "tbcad/ui/scene_sync.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::ui {

struct PointerState {
    Vec2 screen{};  // pixels in the viewport, origin top-left
    bool shift = false;
    bool ctrl = false;
};

// An autosave left behind by a session that did not end cleanly.
struct Recovery {
    std::filesystem::path autosaveFile{};
    std::string originalPath{};  // empty = the drawing had never been saved
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
    std::optional<Recovery> pendingRecovery() const;
    Result<void> recover(const Recovery& r);
    void discardRecovery();
    // Writes settings; removes the autosave. Call on a normal exit.
    void shutdown();

    // ---- document ----------------------------------------------------------------------------
    Document& document();
    const Document& document() const;
    const index::SpatialIndex& spatialIndex() const;
    render::Scene& scene() { return scene_; }
    cmd::UndoStack& undoStack();
    cmd::CommandRunner& runner();
    cmd::Registry& registry() { return registry_; }
    // Incremented whenever a different document is loaded; lets the window drop caches.
    std::uint64_t documentGeneration() const { return generation_; }

    void newDocument();
    Result<void> openFile(const std::filesystem::path& path);
    // Saves to the current file; fails with NotFound-style error text when there is none yet
    // (the window then asks for a name and calls saveAs).
    Result<void> save();
    Result<void> saveAs(const std::filesystem::path& path);
    bool hasFile() const;
    const std::filesystem::path& file() const;
    // Foreign formats. `extension` is lower case with the dot (".dwg"). A read-only format
    // opens as a copy: Save always asks for a new name and never writes the original.
    void setForeignReader(std::string extension, ForeignReadFn read, bool readOnly);
    void setDxfWriter(ForeignWriteFn write) { dxfWriter_ = std::move(write); }
    bool canOpenExtension(std::string_view lowerExtension) const;
    // True while the drawing came from a read-only format and has not been saved as .tbcad.
    bool readOnlyOrigin() const;
    // One-line banner for a read-only drawing; empty otherwise.
    std::string readOnlyBanner() const;
    bool modified() const;
    // "Untitled" or the file name.
    std::string displayName() const;
    // "drawing.tbcad* - TBCAD"
    std::string windowTitle() const;
    std::vector<std::string> recentFiles() const { return settings_.recentFiles(); }

    // ---- undo / redo -------------------------------------------------------------------------
    bool undo();
    bool redo();
    std::string undoLabel() const;  // "Undo Line" ; empty when nothing to undo or a command runs
    std::string redoLabel() const;

    // ---- command line ------------------------------------------------------------------------
    // Submits a line typed in the command line. When idle the first word names a command and the
    // remaining words answer its prompts ("DEVGEN LINES 1000000"); empty text repeats the last
    // command (or, in a prompt, takes the default).
    void submitText(std::string_view text);
    void pressEscape();
    const std::vector<std::string>& history() const { return history_; }
    void print(std::string_view message) override;
    // Prompt line for the command line widget, e.g. "Specify next point or [Close/Undo]: ".
    std::string promptText() const;
    // Command names and aliases beginning with `prefix`, for completion.
    std::vector<std::string> complete(std::string_view prefix) const;

    // ---- viewport ----------------------------------------------------------------------------
    const render::Camera2D& camera() const { return camera_; }
    void setViewportSize(int w, int h);
    void setCamera(const render::Camera2D& cam);
    // `notches` mouse-wheel notches (fractions allowed); factor 1.2 per notch.
    void zoomAtScreen(Vec2 screen, double notches);
    void panPixels(double dx, double dy);
    void zoomExtents();
    void zoomToBox(const BBox2& box);
    void zoomToEntities(const std::vector<EntityId>& ids);
    // Tessellate for the current zoom when it changed enough; call once per frame.
    void updateFrame(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Snap / tracking feedback of the point prompt in progress (tooltip text for the window).
    const cmd::FilterFeedback& feedback() const { return feedback_; }
    // Pixel position of the status-bar coordinates.
    Vec2 cursorWorld() const { return cursorWorld_; }

    // ---- pointer events (from the window) ----------------------------------------------------
    void pointerMoved(const PointerState& p);
    void pointerLeft();
    // A left click that did not turn into a drag.
    void click(const PointerState& p);
    // A double click while no command runs: a text object under the pointer is selected and
    // edited with TEXTEDIT; anything else behaves like a single click.
    void doubleClick(const PointerState& p);
    // A finished rectangle drag from `a` to `b` (screen pixels). Left to right selects what is
    // completely inside (window), right to left what it touches (crossing). Shift removes.
    void dragSelect(Vec2 a, Vec2 b, const PointerState& p);
    // Live selection rectangle shown in the overlay while dragging; nullopt = none.
    void setSelectionDrag(std::optional<std::pair<Vec2, Vec2>> corners) { drag_ = corners; }

    // ---- selection ---------------------------------------------------------------------------
    const std::vector<EntityId>& selection() const override { return selection_; }
    void setSelection(std::vector<EntityId> ids) override;
    void clearSelection() { setSelection({}); }
    void selectAll();
    // Aperture of the pick box in mm at the current zoom.
    double apertureMm() const;

    // ---- overlay -----------------------------------------------------------------------------
    // Crosshair, pick box, command preview, snap marker, selection rectangle and the overlay
    // drawers modules registered. Cleared first.
    void buildOverlay(render::Overlay& overlay, const UiRegistry* ui = nullptr);

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
    // Writes the autosave now when the drawing has unsaved changes. True if one was written.
    bool autosaveNow();

    // ---- cmd::IHost --------------------------------------------------------------------------
    std::optional<std::string> setting(std::string_view key) const override;
    void setSetting(std::string_view key, std::string value) override;
    std::filesystem::path settingsFolder() const override { return paths_.configDir; }
    std::string documentFile() const override { return platform::pathToUtf8(file()); }
    void openPanel(std::string_view panelId) override { panelRequests_.emplace_back(panelId); }
    // Panels asked for since the last call (the window opens them).
    std::vector<std::string> takePanelRequests() { return std::exchange(panelRequests_, {}); }
    cmd::ViewState view() const override;
    void setView(const cmd::ViewState& v) override;
    void setPreview(const DisplayList* list) override;
    void requestUi(std::string_view what) override { uiRequests_.emplace_back(what); }
    // Requests commands made since the last call ("settings"), oldest first.
    std::vector<std::string> takeUiRequests() { return std::exchange(uiRequests_, {}); }
    void promptChanged(const cmd::PromptInfo* prompt) override;
    void feedbackChanged(const cmd::FilterFeedback& feedback) override;

private:
    struct Session;
    class AppObserver;

    void installSession(std::unique_ptr<Document> doc, std::filesystem::path file);
    void resetSessionState();
    void removeAutosaveFiles();
    cmd::RawInput makeRaw(const PointerState& p) const;
    void updateHover(const PointerState& p);
    void applyPick(const std::vector<EntityId>& ids, bool remove);
    void pruneSelection();
    void notifySessionHooks();

    struct ForeignReader {
        ForeignReadFn read{};
        bool readOnly = true;
    };
    std::map<std::string, ForeignReader, std::less<>> foreignReaders_{};
    ForeignWriteFn dxfWriter_{};
    cmd::Registry& registry_;
    cmd::PointFilterChain filterChain_{};
    cmd::CommandLineParser* lineParser_ = nullptr;
    PanelContext makePanelContext();
    std::vector<std::string> uiRequests_{};
    const UiRegistry* ui_ = nullptr;
    platform::AppPaths paths_;
    std::vector<std::string> panelRequests_{};
    platform::Settings settings_;
    platform::Keybindings keybindings_;
    std::vector<std::string> startupWarnings_{};

    render::Scene scene_;
    std::unique_ptr<Session> session_;
    std::uint64_t generation_ = 0;

    render::Camera2D camera_{};
    render::Camera2D lastTessellatedFor_{};
    Vec2 cursorWorld_{};
    bool cursorInside_ = false;
    PointerState lastPointer_{};

    std::vector<EntityId> selection_{};
    std::vector<std::string> history_{};
    std::optional<cmd::PromptInfo> prompt_{};
    cmd::FilterFeedback feedback_{};
    DisplayList preview_{};
    bool hasPreview_ = false;
    std::optional<std::pair<Vec2, Vec2>> drag_{};
    std::optional<OutOfAreaNotice> outOfArea_{};
    bool inUndo_ = false;

    std::chrono::steady_clock::time_point lastAutosave_{};
    std::uint64_t autosavedRevision_ = ~std::uint64_t{0};
    bool autosaveClockStarted_ = false;
};

}  // namespace tbcad::ui
