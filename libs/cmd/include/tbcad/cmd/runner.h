#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "tbcad/cmd/command.h"
#include "tbcad/cmd/registry.h"

namespace tbcad::cmd {

enum class SubmitResult {
    Ignored,          // nothing to do with this input right now
    Rejected,         // the active prompt did not accept it; it asks again
    Accepted,         // delivered to the active prompt
    CommandStarted,   // idle runner: a command was found and started
    CommandFinished,  // the input ended a command (or the command finished at once)
    UnknownCommand,   // idle runner: the text names no command
};

// Drives commands: starts them, feeds them input, handles Esc, commits or rolls back and records
// undo entries. The application owns one; ScriptHost owns one for tests. Single threaded.
class CommandRunner {
public:
    CommandRunner(Document& doc, UndoStack& undo, IHost& host, Registry& registry);
    ~CommandRunner();
    CommandRunner(const CommandRunner&) = delete;
    CommandRunner& operator=(const CommandRunner&) = delete;

    // The text typed so far at a string prompt that can preview it (TEXT, MTEXT).
    void textEdited(const std::string& text);
    // Objects picked so far at the active Select objects prompt (shown as selected).
    const std::vector<EntityId>& collected() const { return collected_; }
    // True while "Select objects:" waits for the points of a typed Window / Crossing / WPolygon /
    // CPolygon / Fence option; the window then sends clicks as points instead of picking.
    bool selectShapePending() const { return shape_ != SelShape::None; }

    // Starts a command by name or alias. Fails when one is already running (use submit with an
    // apostrophe prefix, or startTransparent) or the name is unknown.
    Result<void> start(std::string_view nameOrAlias);
    // Starts a transparent command on top of the running one.
    Result<void> startTransparent(std::string_view nameOrAlias);

    // One unit of user input. Idle: Text names a command (aliases allowed), Enter repeats the last
    // command. Running: answers the active prompt; Text starting with ' starts a transparent
    // command; Cancel (Esc) ends the active prompt with Status::Cancel.
    SubmitResult submit(const Input& input);

    // Cursor moved: updates the rubber-band preview and snap feedback of the active prompt.
    void pointerMoved(const RawInput& raw);
    // Re-reads a prompt whose text follows state outside the command (PointOptions::liveText),
    // e.g. a hatch pattern picked in a panel while the pointer is still over that panel.
    void refreshLiveText();

    bool active() const { return !stack_.empty(); }
    // Prompt of the innermost running command, or null.
    const PromptInfo* prompt() const { return prompt_ ? &*prompt_ : nullptr; }
    const FilterFeedback& feedback() const { return feedback_; }
    std::string_view lastCommand() const { return lastCommand_; }
    Vec2 lastPoint() const { return lastPoint_; }
    void setLastPoint(Vec2 p) { lastPoint_ = p; }

    // Undo and redo through the stack. Refused (false) while a command is running.
    bool undo();
    bool redo();

    // SCRIPT: lines typed one after the other as if the user had entered them, `repeat` times
    // through. They run after the command that queued them finishes. A line that finds a command
    // waiting answers it; an empty line is Enter. Esc drops what is left.
    void queueScript(std::vector<std::string> lines, int repeat = 1);
    // RESUME: queues what Esc dropped from the last script. False when nothing was interrupted.
    bool resumeScript();
    bool scriptRunning() const { return !script_.empty(); }
    // MULTIPLE: `name` runs when the current command ends, and again after every run that asked
    // for input and was not cancelled. Esc (or a failed run) ends the repeating.
    void setMultiple(std::string name) {
        multiple_ = std::move(name);
        multipleHadPrompt_ = true;  // the command setting it asked for the name
    }

    // Not owned; may be null.
    void setFilterChain(PointFilterChain* chain) { chain_ = chain; }
    void setLineParser(CommandLineParser* parser) { parser_ = parser; }
    double ucsAngle() const { return parser_ ? parser_->ucsAngle() : 0; }

private:
    struct Active {
        Command* command = nullptr;
        // The context must outlive the task (the coroutine frame refers to it).
        std::unique_ptr<CommandContext> ctx{};
        Task task{};
    };

    SubmitResult submitInput(const Input& input);
    void pumpScript();
    std::deque<std::string> script_{};
    std::deque<std::string> suspended_{};
    std::string multiple_{};
    bool multipleHadPrompt_ = false;
    int entry_ = 0;  // depth of submit/start calls: scripts are pumped by the outermost one

    Result<void> startImpl(std::string_view nameOrAlias, bool transparent);
    void afterResume();
    void finishTop(bool failed, std::string_view error, bool notify);
    void refreshPrompt();
    SubmitResult answerTop(const Input& input);
    Vec2 filtered(const RawInput& raw, bool click, const std::optional<Vec2>& base,
                  bool snap = true);
    PointParseContext parseContext(const detail::PromptRequest& req) const;
    // What a typed angle at an in-UCS prompt is turned by: the rotation of the current UCS.
    double ucsTurn(const detail::PromptRequest& req) const;

    Document& doc_;
    UndoStack& undo_;
    IHost& host_;
    Registry& registry_;
    std::vector<Active> stack_{};
    std::optional<PromptInfo> prompt_{};
    FilterFeedback feedback_{};
    std::string lastCommand_{};
    Vec2 lastPoint_{};
    Vec2 cursor_{};
    std::vector<EntityId> previousSelection_{};    // what the last Select objects finished with
    std::vector<EntityId> collected_{};            // selection being built at a getSelection prompt
    std::unordered_set<EntityId> collectedSet_{};  // the same ids, for the duplicate check
    // Typed selection options at "Select objects:" (Window, Crossing, WPolygon, CPolygon, Fence,
    // Remove / Add, Undo, BOX, Group, SIngle).
    enum class SelShape { None, Window, Crossing, WPolygon, CPolygon, Fence, Box };
    SelShape shape_ = SelShape::None;
    std::vector<Vec2> shapePts_{};
    bool removeMode_ = false;
    bool singleMode_ = false;    // SIngle: the first pick or window ends Select objects
    bool groupPending_ = false;  // Group: the next text is a group name
    // Typed point modifiers at a point prompt, as in AutoCAD: FROM (a base point, then an offset
    // from it), M2P / MTP (the middle of two points) TT (a temporary tracking point) and TK
    // (tracking: a chain of axis-aligned points). Second: a distance or angle prompt with no base
    // point was answered with a point; the answer is measured from it to the next point, as
    // AutoCAD's "Specify second point".
    enum class PtMod { None, FromBase, FromOffset, Mid1, Mid2, Track, TkFirst, TkNext, Second };
    PtMod ptMod_ = PtMod::None;
    Vec2 ptModA_{};
    // The last segment picked in the running command (from its base to the point), for
    // PointQuery::lastSegment.
    std::optional<std::pair<Vec2, Vec2>> lastSeg_{};
    std::vector<std::vector<EntityId>> selUndo_{};  // collected_ before each batch, newest last
    PointFilterChain* chain_ = nullptr;
    CommandLineParser* parser_ = nullptr;
    DisplayList previewList_{};
};

}  // namespace tbcad::cmd
