#include "tbcad/cmd/registry.h"

namespace tbcad::cmd {

namespace {

class UndoCommand final : public Command {
public:
    std::string_view name() const override { return "UNDO"; }
    std::vector<std::string> defaultAliases() const override { return {"U"}; }
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
    (void)registry.add(std::make_unique<UndoCommand>());
    (void)registry.add(std::make_unique<RedoCommand>());
    (void)registry.add(std::make_unique<UndoBeginCommand>());
    (void)registry.add(std::make_unique<UndoEndCommand>());
    (void)registry.add(std::make_unique<UndoMarkCommand>());
    (void)registry.add(std::make_unique<UndoBackCommand>());
    (void)registry.add(std::make_unique<AliasesCommand>());
}

}  // namespace tbcad::cmd
