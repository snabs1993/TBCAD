#pragma once

#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::block_cmds {

// One line of the block list panel.
struct BlockRow {
    BlockId id{};
    std::string name{};
    std::size_t objects = 0;     // entities in the definition
    std::size_t references = 0;  // Inserts of it in model space and in other blocks
    Vec2 base{};
};

// All named blocks (anonymous ones are hidden), sorted by name, case-insensitively. Names that
// contain `filter` (case-insensitive) only; an empty filter keeps everything.
std::vector<BlockRow> blockRows(const Document& doc, const std::string& filter = {});

}  // namespace tbcad::block_cmds
