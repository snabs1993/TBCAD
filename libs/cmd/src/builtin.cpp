#include <limits>
#include <string>

#include "tbcad/cmd/registry.h"

namespace tbcad::cmd {

namespace {

// U: undo one step.
class UCommand final : public Command {
public:
    std::string_view name() const override { return "U"; }
    std::string_view description() const override { return "Undo the last command"; }
    Task run(CommandContext& ctx) override {
        std::string label = ctx.undoStack().undoLabel();
        if (ctx.undoStack().undo(ctx.doc()))
            ctx.message("Undo " + label);
        else
            ctx.message("Nothing to undo.");
        co_return;
    }
};

// UNDO as in AutoCAD: a number of steps, or the group and mark options.
class UndoCommand final : public Command {
public:
    std::string_view name() const override { return "UNDO"; }
    std::string_view description() const override {
        return "Undo a number of steps, or set groups and marks";
    }
    Task run(CommandContext& ctx) override {
        UndoStack& st = ctx.undoStack();
        IntOptions io;
        io.keywords = {"Auto", "Control", "BEgin", "End", "Mark", "Back"};
        io.defaultValue = 1;
        io.min = 1;
        auto r = co_await ctx.getInteger(
            "Enter the number of operations to undo or [Auto/Control/BEgin/End/Mark/Back] <1>", io);
        if (r.status == Status::Ok) {
            long long done = 0;
            std::string last;
            for (; done < r.value; ++done) {
                last = st.undoLabel();
                if (!st.undo(ctx.doc()))
                    break;
            }
            if (done == 0)
                ctx.message(st.blockedReason().empty() ? "Nothing to undo." : st.blockedReason());
            else if (done == 1)
                ctx.message("Undo " + last);
            else
                ctx.message("Undid " + std::to_string(done) + " steps.");
            co_return;
        }
        if (r.status != Status::Keyword)
            co_return;
        if (r.keyword == "BEgin") {
            st.beginGroup("Group");
        } else if (r.keyword == "End") {
            if (!st.endGroup())
                ctx.message("No undo group is open.");
        } else if (r.keyword == "Mark") {
            st.mark();
        } else if (r.keyword == "Back") {
            const std::size_t n = st.undoToMark(ctx.doc());
            if (n != UndoStack::npos) {
                ctx.message("Undid " + std::to_string(n) + " step(s) back to the mark.");
                co_return;
            }
            // No mark: AutoCAD offers to undo everything.
            KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = "Yes";
            auto a = co_await ctx.getKeyword("This will undo everything. OK? [Yes/No] <Yes>", yn);
            if (!a.ok() || a.keyword != "Yes")
                co_return;
            std::size_t all = 0;
            while (st.undo(ctx.doc()))
                ++all;
            ctx.message("Undid " + std::to_string(all) + " step(s).");
        } else if (r.keyword == "Auto" || r.keyword == "Control") {
            // TBCAD always keeps the whole history (within its memory limit) and every command
            // is one step, so these are accepted for scripts written for AutoCAD.
            KeywordOptions ko;
            ko.keywords = r.keyword == "Auto" ? std::vector<std::string>{"ON", "OFF"}
                                              : std::vector<std::string>{"All", "None", "One"};
            ko.defaultKeyword = ko.keywords.front();
            auto a = co_await ctx.getKeyword(
                r.keyword == "Auto" ? "Enter UNDO Auto mode [ON/OFF] <ON>"
                                    : "Enter an UNDO control option [All/None/One] <All>",
                ko);
            if (a.ok() && a.keyword != "ON" && a.keyword != "All")
                ctx.message("TBCAD keeps the full undo history; the setting is not changed.");
        }
    }
};

class RedoCommand final : public Command {
public:
    std::string_view name() const override { return "REDO"; }
    std::string_view description() const override { return "Redo the last undone command"; }
    Task run(CommandContext& ctx) override {
        std::string label = ctx.undoStack().redoLabel();
        if (ctx.undoStack().redo(ctx.doc()))
            ctx.message("Redo " + label);
        else
            ctx.message("Nothing to redo.");
        co_return;
    }
};

// MREDO: redo a number of steps, or All.
class MRedoCommand final : public Command {
public:
    std::string_view name() const override { return "MREDO"; }
    std::string_view description() const override { return "Redo several undone steps at once"; }
    Task run(CommandContext& ctx) override {
        UndoStack& st = ctx.undoStack();
        IntOptions io;
        io.keywords = {"All", "Last"};
        io.defaultValue = 1;
        io.min = 1;
        auto r = co_await ctx.getInteger("Enter the number of actions to redo or [All/Last]", io);
        long long want = 0;
        if (r.status == Status::Ok)
            want = r.value;
        else if (r.status == Status::Keyword)
            want = r.keyword == "All" ? std::numeric_limits<long long>::max() : 1;
        else
            co_return;
        long long done = 0;
        for (; done < want && st.redo(ctx.doc()); ++done) {
        }
        if (done == 0)
            ctx.message("Nothing to redo.");
        else
            ctx.message("Redid " + std::to_string(done) + (done == 1 ? " step." : " steps."));
    }
};

class UndoBeginCommand final : public Command {
public:
    std::string_view name() const override { return "UNDOBEGIN"; }
    std::string_view description() const override {
        return "Start a group: following commands undo as one step";
    }
    Task run(CommandContext& ctx) override {
        ctx.undoStack().beginGroup("Group");
        co_return;
    }
};

class UndoEndCommand final : public Command {
public:
    std::string_view name() const override { return "UNDOEND"; }
    std::string_view description() const override { return "End the group started by UNDOBEGIN"; }
    Task run(CommandContext& ctx) override {
        if (!ctx.undoStack().endGroup())
            ctx.message("No undo group is open.");
        co_return;
    }
};

class UndoMarkCommand final : public Command {
public:
    std::string_view name() const override { return "UNDOMARK"; }
    std::string_view description() const override { return "Mark this place in the undo history"; }
    Task run(CommandContext& ctx) override {
        if (ctx.undoStack().undoCount() == 0 && !ctx.undoStack().inGroup())
            ctx.message("Nothing to mark: the undo history is empty.");
        else
            ctx.undoStack().mark();
        co_return;
    }
};

class UndoBackCommand final : public Command {
public:
    std::string_view name() const override { return "UNDOBACK"; }
    std::string_view description() const override {
        return "Undo back to the mark set by UNDOMARK";
    }
    Task run(CommandContext& ctx) override {
        const std::size_t n = ctx.undoStack().undoToMark(ctx.doc());
        if (n == UndoStack::npos)
            ctx.message("No mark: use UNDOMARK first.");
        else
            ctx.message("Undid " + std::to_string(n) + " step(s) back to the mark.");
        co_return;
    }
};

class AliasesCommand final : public Command {
public:
    std::string_view name() const override { return "ALIASES"; }
    std::string_view description() const override { return "Reload aliases.json and acad.pgp"; }
    Task run(CommandContext& ctx) override {
        const auto folder = ctx.host().settingsFolder();
        AliasReport r;
        if (folder.empty())
            r = ctx.registry().loadUserAliases({});
        else
            r = ctx.registry().loadUserAliases(folder / "aliases.json");
        for (const std::string& e : r.errors)
            ctx.message("Alias error: " + e);
        for (const std::string& w : r.warnings)
            ctx.message("Alias conflict: " + w);
        ctx.message("Aliases reloaded: " + std::to_string(r.userAliases) + " user alias(es).");
        co_return;
    }
};

}  // namespace

void registerBuiltinCommands(Registry& registry) {
    // Registering twice is a programmer error that add() reports; ignore it here so the call is
    // idempotent for hosts that register the built-ins and then the generated list.
    (void)registry.add(std::make_unique<UCommand>());
    (void)registry.add(std::make_unique<UndoCommand>());
    (void)registry.add(std::make_unique<RedoCommand>());
    (void)registry.add(std::make_unique<MRedoCommand>());
    (void)registry.add(std::make_unique<UndoBeginCommand>());
    (void)registry.add(std::make_unique<UndoEndCommand>());
    (void)registry.add(std::make_unique<UndoMarkCommand>());
    (void)registry.add(std::make_unique<UndoBackCommand>());
    (void)registry.add(std::make_unique<AliasesCommand>());
}

}  // namespace tbcad::cmd
