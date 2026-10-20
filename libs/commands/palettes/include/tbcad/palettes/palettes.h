#pragma once

// Tool palettes. A tool is a saved way to start a command: insert a block, start a hatch with a
// pattern, or run any command line, optionally on a chosen layer. Palettes are stored as JSON in
// the settings folder (palettes.json) and edited from the PALETTES panel.

#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::palettes {

enum class ToolKind { Block, Hatch, Command };

struct Tool {
    std::string name{};
    ToolKind kind = ToolKind::Command;
    std::string layer{};  // layer path to make current first; empty = leave as is
    // Block: the block name, and the drawing file it comes from when the open drawing may not
    // have it yet.
    std::string block{};
    std::string file{};
    double scale = 1;       // block scale or hatch pattern scale
    double angle = 0;       // block rotation or hatch pattern angle, degrees
    std::string pattern{};  // hatch pattern name
    std::string
        command{};  // Command: the command line to run (may be several lines separated by |)
};

struct Palette {
    std::string name{};
    std::vector<Tool> tools{};
};

// The palettes shipped with the program, used until the user changes anything.
std::vector<Palette> defaultPalettes();

class PaletteStore {
public:
    explicit PaletteStore(std::filesystem::path file) : file_(std::move(file)) {}

    // Reads the file; a missing file gives the default palettes, a damaged one too (with a warning
    // in `warning()`).
    void load();
    // Writes the file atomically. Returns an error text, empty on success.
    std::string save() const;

    std::vector<Palette>& palettes() { return palettes_; }
    const std::vector<Palette>& palettes() const { return palettes_; }
    const std::string& warning() const { return warning_; }

    Palette* find(const std::string& name);
    // Returns an error text, empty on success. Names are unique (case-insensitive).
    std::string addPalette(const std::string& name);
    std::string renamePalette(const std::string& from, const std::string& to);
    std::string removePalette(const std::string& name);
    std::string addTool(const std::string& palette, Tool tool);
    std::string removeTool(const std::string& palette, const std::string& tool);
    // Replaces the tool named `tool` (its Properties), checked like addTool; the new name may be
    // the old one.
    std::string replaceTool(const std::string& palette, const std::string& tool, Tool with);
    // Moves a tool within its palette by `delta` places (negative = earlier).
    std::string moveTool(const std::string& palette, const std::string& tool, int delta);

private:
    std::filesystem::path file_;
    std::vector<Palette> palettes_{};
    std::string warning_{};
};

// The command-line answers that start a tool. The first line is the command, the rest answer its
// prompts; the last prompt (the point to pick) stays open for the user. `doc` tells whether a
// block still has to be copied from its file first. An empty string in the list means "send an
// empty line" (end of a command).
std::vector<std::string> toolLines(const Tool& tool, const Document& doc);

// Tools that make more objects like the given ones ("Add selected objects to the palette"): an
// insert gives a Block tool (its scale and rotation; `drawingFile` as the file it comes from), a
// hatch a Hatch tool (pattern, scale, angle), anything else a Command tool that starts the command
// drawing that kind of object. Each tool sets the object's layer. One tool per distinct result,
// named after the block, pattern or command.
std::vector<Tool> toolsFromObjects(const Document& doc, const std::vector<EntityId>& ids,
                                   const std::string& drawingFile = {});

void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::palettes
