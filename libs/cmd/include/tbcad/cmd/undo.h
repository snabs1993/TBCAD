#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::cmd {

// Approximate heap size of a change set (snapshots plus table rows). Shared snapshot storage is
// counted once.
std::size_t estimateBytes(const ChangeSet& changes);

// History of committed commands. One entry per command (or per UNDO BEGIN/END group); an entry may
// consist of several change sets (checkpoints inside a command).
//
// The stack is the only thing that should call Document::apply for undo and redo, and it assumes
// the document is not changed behind its back.
class UndoStack {
public:
    static constexpr std::size_t kDefaultLimitBytes = std::size_t{512} * 1024 * 1024;

    // A quarter of the machine's memory, between the default and 2 GB: a big drawing's edits take
    // 100+ MB each, and the default would keep only three of them.
    static std::size_t machineLimitBytes();

    explicit UndoStack(std::size_t limitBytes = kDefaultLimitBytes) : limit_(limitBytes) {}

    // Records a finished command. Empty change sets are skipped; an entry with nothing left is
    // not recorded. Clears the redo history. Inside a group (beginGroup) the sets are collected
    // and recorded as a single entry at endGroup. `bytes` overrides the size estimate (tests).
    void push(std::string label, std::vector<ChangeSet> sets, std::size_t bytes = 0);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // Menu labels: "Line", "Move"... Empty when there is nothing to undo / redo.
    std::string undoLabel() const { return undo_.empty() ? std::string{} : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string{} : redo_.back().label; }
    // Size estimate of the step undo / redo would apply next (0 when there is none): a hint for
    // how long it will take.
    std::size_t nextUndoBytes() const { return undo_.empty() ? 0 : undo_.back().bytes; }
    std::size_t nextRedoBytes() const { return redo_.empty() ? 0 : redo_.back().bytes; }
    std::size_t undoCount() const { return undo_.size(); }
    std::size_t redoCount() const { return redo_.size(); }
    // Labels, oldest first.
    std::vector<std::string> undoLabels() const;

    // Return false when there is nothing to do. undo() first closes an open group (all levels),
    // so the group is undone whole; redo() refuses while a group is open.
    bool undo(Document& doc);
    bool redo(Document& doc);

    // Macros: everything pushed between beginGroup and the matching endGroup becomes one undo
    // step. Groups nest; only the outermost label is used.
    void beginGroup(std::string label);
    bool endGroup();  // false when no group is open
    bool inGroup() const { return groupDepth_ > 0; }

    // UNDO Mark / Back: remembers the current place in the history; undoToMark steps back to the
    // latest mark that is still ahead of the history's start and removes it. Returns the number of
    // steps undone, or npos when there is no mark (or a group is open / a step was refused).
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    void mark();
    bool hasMark() const { return !marks_.empty(); }
    std::size_t undoToMark(Document& doc);

    void clear();

    // Shared drawings: before an entry is undone or redone the guard may look at the document and
    // the entry's change sets (in the order they were recorded) and refuse by returning a message,
    // for instance because somebody else changed the same things since. A refused entry is dropped
    // (it can never apply again), undo()/redo() return false and blockedReason() says why.
    using Guard =
        std::function<std::string(const Document&, std::span<const ChangeSet>, Direction)>;
    void setGuard(Guard guard) { guard_ = std::move(guard); }
    const std::string& blockedReason() const { return blocked_; }

    // Steps dropped from the old end to stay inside the limit, since the stack was made.
    std::size_t droppedSteps() const { return dropped_; }
    std::size_t usedBytes() const { return used_; }
    std::size_t limitBytes() const { return limit_; }
    void setLimitBytes(std::size_t limit);

    // Called with a message when old entries are dropped to stay under the limit.
    void setMessageSink(std::function<void(std::string)> sink) { sink_ = std::move(sink); }

private:
    struct Entry {
        std::string label{};
        std::vector<ChangeSet> sets{};
        std::size_t bytes = 0;
    };
    void record(Entry e);
    void trim();

    std::deque<Entry> undo_{};
    std::vector<Entry> redo_{};
    std::size_t used_ = 0;
    std::size_t limit_;
    std::vector<std::size_t> marks_{};  // history depths (undo_.size()) of the marks, ascending
    int groupDepth_ = 0;
    Entry group_{};
    std::size_t dropped_ = 0;
    std::function<void(std::string)> sink_{};
    Guard guard_{};
    std::string blocked_{};
};

}  // namespace tbcad::cmd
