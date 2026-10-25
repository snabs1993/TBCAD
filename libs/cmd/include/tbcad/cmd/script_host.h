#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/cmd/runner.h"

namespace tbcad::cmd {

// One scripted user action. Implicitly built from text: "0,0", "@100,0", "C"; "" is Enter.
struct ScriptItem {
    Input input{};

    ScriptItem(const char* text)  // NOLINT: implicit by design
        : input(*text ? Input::typed(text) : Input::enter()) {}
    ScriptItem(std::string text)  // NOLINT
        : input(text.empty() ? Input::enter() : Input::typed(std::move(text))) {}
    ScriptItem(Input i) : input(std::move(i)) {}  // NOLINT

    static ScriptItem esc() { return ScriptItem(Input::cancel()); }
    static ScriptItem enter() { return ScriptItem(Input::enter()); }
    static ScriptItem click(Vec2 p) { return ScriptItem(Input::click(p)); }
    static ScriptItem pick(EntityId id, Vec2 at = {}, bool shift = false) {
        return ScriptItem(Input::pick(id, at, shift));
    }
    static ScriptItem select(std::vector<EntityId> ids) {
        return ScriptItem(Input::select(std::move(ids)));
    }
};

struct ScriptResult {
    bool started = false;      // the command was found and started
    bool finished = false;     // the command is no longer running
    std::size_t consumed = 0;  // items fed before the command finished
    std::size_t rejected = 0;  // items a prompt refused
    std::string error{};       // set when `started` is false
};

// Headless host: feeds scripted input to commands so every command can be tested without a UI.
//
//   Document doc; Registry reg; registerBuiltinCommands(reg); reg.add(...);
//   ScriptHost host(doc, reg);
//   auto r = host.run("LINE", {"0,0", "@100,0", ScriptItem::enter()});
//   CHECK(r.finished);
//   host.undo();
class ScriptHost : public IHost {
public:
    ScriptHost(Document& doc, Registry& registry);
    ~ScriptHost() override;

    // Starts `command` (name or alias) and feeds the items in order. Stops early when the
    // command finishes; remaining items are left unused (`consumed` says how many were used).
    // If items run out first the command stays active: continue with feed().
    ScriptResult run(std::string_view command, const std::vector<ScriptItem>& items);
    // Feeds more items to the running command.
    ScriptResult feed(const std::vector<ScriptItem>& items);

    bool undo() { return runner_.undo(); }
    bool redo() { return runner_.redo(); }

    Document& document() { return doc_; }
    UndoStack& undoStack() { return undo_; }
    CommandRunner& runner() { return runner_; }
    Registry& registry() { return registry_; }
    PointFilterChain& filters() { return chain_; }

    // Everything printed so far.
    const std::vector<std::string>& log() const { return log_; }
    bool logContains(std::string_view needle) const;
    // What commands asked the window to do (requestUi), in order.
    const std::vector<std::string>& uiRequests() const { return uiRequests_; }
    void requestUi(std::string_view what) override { uiRequests_.emplace_back(what); }
    bool queueScript(std::vector<std::string> lines, int repeat) override {
        runner_.queueScript(std::move(lines), repeat);
        return true;
    }
    bool resumeScript() override { return runner_.resumeScript(); }
    bool repeatCommand(const std::string& name) override {
        runner_.setMultiple(name);
        return true;
    }
    void clearLog() { log_.clear(); }

    // Last preview list set by the active prompt (empty when none).
    const DisplayList& preview() const { return preview_; }
    // Prompts seen, oldest first (display text); useful to assert on the flow.
    const std::vector<std::string>& prompts() const { return prompts_; }
    // The text box a string prompt asked for, if it did (the last one shown).
    const std::optional<TextEditorHint>& textEditor() const { return textEditor_; }

    // IHost
    void print(std::string_view message) override { log_.emplace_back(message); }
    const std::vector<EntityId>& selection() const override { return selection_; }
    void setSelection(std::vector<EntityId> ids) override { selection_ = std::move(ids); }
    std::optional<std::string> setting(std::string_view key) const override;
    double ucsAngle() const override { return runner_.ucsAngle(); }
    void setSetting(std::string_view key, std::string value) override;
    std::filesystem::path settingsFolder() const override { return settingsFolder_; }
    void setSettingsFolder(std::filesystem::path p) { settingsFolder_ = std::move(p); }
    ViewState view() const override { return view_; }
    void setView(const ViewState& v) override { view_ = v; }
    // Tests: answers entitiesInWindow() with `fn` (without it the host reports "no index").
    using WindowQuery = std::function<std::vector<EntityId>(const BBox2&)>;
    void setWindowQuery(WindowQuery fn) { windowQuery_ = std::move(fn); }
    std::optional<std::vector<EntityId>> entitiesInWindow(const BBox2& window) const override {
        if (!windowQuery_)
            return std::nullopt;
        return windowQuery_(window);
    }
    // Tests: pretend a layout sheet is shown, with `model` as the spatial index of model space.
    void showLayout(std::string name, const index::SpatialIndex* model) {
        layoutName_ = std::move(name);
        modelIndex_ = model;
    }
    // Tests: new objects go into this block (a layout sheet's), like a host showing a layout.
    void setDrawingOwner(BlockId owner) { drawingOwner_ = owner; }
    BlockId drawingOwner() const override { return drawingOwner_; }
    std::string activeLayoutName() const override { return layoutName_; }
    const index::SpatialIndex* modelSpatialIndex() const override { return modelIndex_; }
    // Tests: the box viewExtents() reports (empty by default).
    void setViewExtents(const BBox2& box) { extents_ = box; }
    BBox2 viewExtents() const override { return extents_; }
    void setPreview(const DisplayList* list) override;
    void promptChanged(const PromptInfo* prompt) override;

private:
    BBox2 extents_{};
    std::vector<std::string> uiRequests_{};
    ScriptResult pump(const std::vector<ScriptItem>& items, ScriptResult r);

    Document& doc_;
    Registry& registry_;
    UndoStack undo_{};
    PointFilterChain chain_{};
    CommandRunner runner_;
    std::vector<std::string> log_{};
    std::vector<std::string> prompts_{};
    std::optional<TextEditorHint> textEditor_{};
    std::vector<EntityId> selection_{};
    std::vector<std::pair<std::string, std::string>> settings_{};
    std::filesystem::path settingsFolder_{};
    ViewState view_{};
    WindowQuery windowQuery_{};
    std::string layoutName_{};
    BlockId drawingOwner_{};
    const index::SpatialIndex* modelIndex_ = nullptr;
    DisplayList preview_{};
};

}  // namespace tbcad::cmd
