#pragma once

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

    // Not owned; may be null.
    void setFilterChain(PointFilterChain* chain) { chain_ = chain; }
    void setLineParser(CommandLineParser* parser) { parser_ = parser; }

private:
    struct Active {
        Command* command = nullptr;
        // The context must outlive the task (the coroutine frame refers to it).
        std::unique_ptr<CommandContext> ctx{};
        Task task{};
    };

    Result<void> startImpl(std::string_view nameOrAlias, bool transparent);
    void afterResume();
    void finishTop(bool failed, std::string_view error, bool notify);
    void refreshPrompt();
    SubmitResult answerTop(const Input& input);
    Vec2 filtered(const RawInput& raw, bool click, const std::optional<Vec2>& base);
    PointParseContext parseContext(const detail::PromptRequest& req) const;

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
    PointFilterChain* chain_ = nullptr;
    CommandLineParser* parser_ = nullptr;
    DisplayList previewList_{};
};

}  // namespace tbcad::cmd
