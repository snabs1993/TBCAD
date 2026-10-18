#pragma once

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/input.h"
#include "tbcad/cmd/task.h"
#include "tbcad/cmd/undo.h"
#include "tbcad/core/core.h"

namespace tbcad::cmd {

class CommandContext;
class CommandRunner;
class Registry;

// A command. Instances are owned by the Registry, live for the whole session and are shared by
// every run, so they must not keep per-run state in members; keep it in locals of run().
class Command {
public:
    virtual ~Command() = default;

    // Upper-case ASCII, e.g. "LINE". Matching is case-insensitive.
    virtual std::string_view name() const = 0;
    // Built-in short names, e.g. {"L"}. Users override them in aliases.json.
    virtual std::vector<std::string> defaultAliases() const { return {}; }
    // One line for the command list / help.
    virtual std::string_view description() const { return {}; }
    // Transparent commands (ZOOM, PAN ...) may be started while another command waits for input
    // (typed with a leading apostrophe, or started by the UI) and share its transaction.
    virtual bool isTransparent() const { return false; }

    virtual Task run(CommandContext& ctx) = 0;
};

namespace detail {

enum class PromptKind { Point, Distance, Angle, Real, Integer, String, Keyword, Selection, Entity };

// A prompt as stored by the context while the command is suspended on it.
struct PromptRequest {
    PromptKind kind = PromptKind::Point;
    std::string text{};
    std::vector<std::string> keywords{};
    std::vector<std::pair<std::string, std::string>> keywordAliases{};  // typed -> keyword
    std::optional<Vec2> base{};
    PreviewFn preview{};
    TextPreviewFn textPreview{};
    EntityFilterFn filter{};
    std::optional<double> defaultReal{};
    std::optional<long long> defaultInt{};
    std::optional<std::string> defaultText{};
    std::optional<double> min{};
    std::optional<double> max{};
    bool allowNone = true;
    bool freeText = false;
    bool allowEmptyString = false;
    bool allowPreselection = true;
    bool single = false;
    bool allowZero = true;
    bool allowNegative = true;
};

// Superset of all result types; the awaiter picks the fields its result needs.
struct PromptAnswer {
    Status status = Status::Cancel;
    Vec2 point{};
    double real = 0;
    long long integer = 0;
    std::string text{};
    std::string keyword{};
    EntityId entity{};
    std::vector<EntityId> ids{};
    bool shift = false;
};

template <class R>
R toResult(PromptAnswer&& a);
template <>
inline PointResult toResult<PointResult>(PromptAnswer&& a) {
    return {a.status, a.point, std::move(a.keyword)};
}
template <>
inline RealResult toResult<RealResult>(PromptAnswer&& a) {
    return {a.status, a.real, std::move(a.keyword)};
}
template <>
inline IntResult toResult<IntResult>(PromptAnswer&& a) {
    return {a.status, a.integer, std::move(a.keyword)};
}
template <>
inline StringResult toResult<StringResult>(PromptAnswer&& a) {
    return {a.status, std::move(a.text)};
}
template <>
inline KeywordResult toResult<KeywordResult>(PromptAnswer&& a) {
    return {a.status, std::move(a.keyword)};
}
template <>
inline EntityResult toResult<EntityResult>(PromptAnswer&& a) {
    return {a.status, a.entity, a.point, std::move(a.keyword), a.shift};
}
template <>
inline SelectionResult toResult<SelectionResult>(PromptAnswer&& a) {
    return {a.status, std::move(a.ids), std::move(a.keyword)};
}

}  // namespace detail

// Handed to Command::run. Exactly one context per running command.
class CommandContext {
public:
    CommandContext(Document& doc, IHost& host, UndoStack& undo, Registry& registry,
                   std::string commandName, CommandContext* parent = nullptr);
    ~CommandContext();
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    Document& doc() { return doc_; }
    IHost& host() { return host_; }
    UndoStack& undoStack() { return undo_; }
    Registry& registry() { return registry_; }
    const std::string& commandName() const { return name_; }

    // The command's transaction, opened on first use. Committed when run() returns normally,
    // rolled back when the command was cancelled (unless commitOnCancel(true)) or threw. A
    // transparent command running inside another command shares the parent's transaction.
    Transaction& tx();

    // Writes a line to the command line / log.
    void message(std::string text) { host_.print(text); }

    // ---- prompts: `auto r = co_await ctx.getPoint("Specify first point");` ---------------------
    template <class R>
    class Awaiter {
    public:
        Awaiter(CommandContext& ctx, detail::PromptRequest req) : ctx_(ctx), req_(std::move(req)) {}
        bool await_ready() { return ctx_.tryImmediate(req_); }
        void await_suspend(std::coroutine_handle<> h) { ctx_.suspendOn(std::move(req_), h); }
        R await_resume() { return detail::toResult<R>(ctx_.takeAnswer()); }

    private:
        CommandContext& ctx_;
        detail::PromptRequest req_;
    };

    [[nodiscard]] Awaiter<PointResult> getPoint(std::string prompt, PointOptions opts = {});
    [[nodiscard]] Awaiter<RealResult> getDistance(std::string prompt, DistanceOptions opts = {});
    // Result in radians.
    [[nodiscard]] Awaiter<RealResult> getAngle(std::string prompt, AngleOptions opts = {});
    [[nodiscard]] Awaiter<RealResult> getReal(std::string prompt, RealOptions opts = {});
    [[nodiscard]] Awaiter<IntResult> getInteger(std::string prompt, IntOptions opts = {});
    [[nodiscard]] Awaiter<StringResult> getString(std::string prompt, StringOptions opts = {});
    [[nodiscard]] Awaiter<KeywordResult> getKeyword(std::string prompt, KeywordOptions opts);
    // Noun-verb: returns the host's pre-selection first when allowed and non-empty.
    [[nodiscard]] Awaiter<SelectionResult> getSelection(std::string prompt,
                                                        SelectionOptions opts = {});
    [[nodiscard]] Awaiter<EntityResult> getEntity(std::string prompt, EntityOptions opts = {});

    // ---- sub-steps ---------------------------------------------------------------------------
    // Commits what the command did so far and remembers that state. rollbackToCheckpoint()
    // returns the document to the most recent remembered state and forgets it, so LINE can call
    // checkpoint() before each segment and rollbackToCheckpoint() for its Undo option.
    // Observers see both steps. Not allowed in transparent commands.
    void checkpoint();
    bool rollbackToCheckpoint();  // false when there is no checkpoint
    std::size_t checkpointCount() const { return marks_.size(); }
    // Commits what the command has added since its last checkpoint, so the drawing shows it while
    // the command waits for the next point. Only for commands that use checkpoints; Undo still
    // removes it. Called by the runner whenever the command asks for more input.
    void publishPending();

    // True once the user pressed Esc at one of this command's prompts. Later prompts answer
    // Cancel immediately, so a command that keeps looping still terminates.
    bool cancelled() const { return cancelled_; }
    // The last point the user entered or clicked in this session, as of the most recent answer
    // (so after a distance prompt answered by a click it is that click). Lets a command recover
    // the direction of a point-or-number answer.
    Vec2 lastPoint() const { return lastPoint_; }
    // By default Esc discards everything the command did. LINE-like commands keep the segments
    // already drawn: call commitOnCancel() once at the start.
    void commitOnCancel(bool keep = true) { commitOnCancel_ = keep; }
    // Label for the Undo menu (defaults to the command name).
    void setUndoLabel(std::string label) { label_ = std::move(label); }

    // ---- used by CommandRunner -----------------------------------------------------------------
    bool hasPending() const { return pending_.has_value(); }
    const detail::PromptRequest* pending() const { return pending_ ? &*pending_ : nullptr; }
    // Stores the answer and returns the coroutine to resume.
    std::coroutine_handle<> deliver(detail::PromptAnswer answer);
    void markCancelled() { cancelled_ = true; }
    void setLastPoint(Vec2 p) { lastPoint_ = p; }
    // Ends the command: commits or rolls back. Returns true if an undo entry was recorded.
    bool finish(bool failed);
    CommandContext* parent() const { return parent_; }

private:
    bool tryImmediate(detail::PromptRequest& req);
    void suspendOn(detail::PromptRequest req, std::coroutine_handle<> h);
    detail::PromptAnswer takeAnswer() { return std::move(answer_); }
    void commitOpen();
    void undoSegmentsTo(std::size_t count);

    Document& doc_;
    IHost& host_;
    UndoStack& undo_;
    Registry& registry_;
    std::string name_;
    std::string label_;
    CommandContext* parent_;

    std::optional<Transaction> tx_{};
    std::vector<ChangeSet> segments_{};
    std::vector<std::size_t> marks_{};  // segments_.size() at each checkpoint
    bool cancelled_ = false;
    Vec2 lastPoint_{};
    bool commitOnCancel_ = false;

    std::optional<detail::PromptRequest> pending_{};
    std::coroutine_handle<> waiting_{};
    detail::PromptAnswer answer_{};
};

}  // namespace tbcad::cmd
