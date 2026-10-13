#pragma once

// Undo and redo in a shared drawing. A person's undo stack holds only their own edits, so undoing
// is "put back what I had before". That is only right while nobody else has changed the same
// things since: otherwise the undo would quietly throw their work away. checkUndoRedo is what
// cmd::UndoStack::setGuard is given; it compares the drawing with the images the entry would
// restore over.
//
//   Undo: every thing the entry touched must still look as the entry left it.
//   Redo: every thing must still look as it did before the entry.
//
// Returns an empty string when the step is safe, else a sentence for the user. Table rows and
// entities are compared; the header and draw order follow the entities they belong to.

#include <span>
#include <string>

#include "tbcad/core/document.h"

namespace tbcad::collab {

std::string checkUndoRedo(const Document& doc, std::span<const ChangeSet> sets,
                          Direction direction);

}  // namespace tbcad::collab
