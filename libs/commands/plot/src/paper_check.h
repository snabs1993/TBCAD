// PAPERCHECK / PAPERFIT and the Paper Check panel share what was checked last.
#pragma once
#include <vector>

#include "tbcad/layout/paper_size.h"

namespace tbcad::plot {

struct PaperCheckState {
    bool selectionOnly = false;     // check `scope`, not the whole drawing
    std::vector<EntityId> scope{};  // the objects selected when PAPERCHECK ran
    bool colours = true;            // outline the checked objects by their result
};
PaperCheckState& paperCheckState();

// The objects a check covers: `selected` when not empty, else the whole drawing.
std::vector<layout::PaperCheckItem> checkScope(const Document& doc,
                                               const std::vector<EntityId>& selected);
// "12 checked: 9 OK, 2 made for another scale, 1 too small".
std::string paperCheckSummary(const std::vector<layout::PaperCheckItem>& items);

}  // namespace tbcad::plot
