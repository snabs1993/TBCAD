#pragma once

#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/transform/grips.h"

// Multifunctional grips: hovering a grip offers more than "stretch". The list depends on the kind
// of object and on which of its grips it is (a polyline vertex, the midpoint of a segment, the end
// of a line ...). The UI shows `gripActions` in a small menu; choosing one runs
// `gripActionCommand`. The same keyword can be typed after GRIPEDIT x,y.
namespace tbcad::transform {

enum class GripAction {
    Stretch,
    AddVertex,      // polyline: a new vertex (on a segment grip it splits that segment)
    RemoveVertex,   // polyline vertex
    ConvertToArc,   // straight polyline segment -> arc through the point
    ConvertToLine,  // arc polyline segment -> straight
    Lengthen,       // line end: slide along the line; arc end: turn about the centre
    Radius,         // arc middle / circle quadrant: new radius
    ResetText,      // dimension text goes back to its automatic place
    Continue,       // linear dimension: start a continued dimension from this extension line
    Baseline,       // linear dimension: start a baseline dimension from this extension line
    FlipArrow,      // dimension: the arrowhead at this end turns the other way
    // Linear dimension text grip, as AutoCAD's: the text is dragged with the dimension line
    // (DIMTMOVE 0), on its own (2) or with a leader (1), or put above the dimension line (DIMTAD 1)
    // or centred on it (0). Each changes that dimension's own style (an override, "<style>$N").
    MoveWithLine,
    MoveTextOnly,
    MoveWithLeader,
    AboveLine,
    CenterVertically,
};

// Menu text ("Add vertex") and the keyword GRIPEDIT understands ("AddVertex").
const char* gripActionLabel(GripAction a);
const char* gripActionKeyword(GripAction a);
// Inverse of gripActionKeyword (case-insensitive); false for an unknown word.
bool gripActionFromKeyword(const std::string& word, GripAction& out);

// Actions offered for a grip, Stretch first. Empty when the grip does not exist.
std::vector<GripAction> gripActions(const Document& doc, const Grip& grip);

// True when the action needs a point from the user (RemoveVertex, ConvertToLine, ResetText,
// Continue, Baseline and FlipArrow do not).
bool gripActionNeedsPoint(GripAction a);

// The entity after the action at grip `index`; `target` is the point the user gave (ignored when
// the action needs none). Errors say why the action cannot be done.
Result<Entity> applyGripAction(const Entity& e, int index, GripAction action, Vec2 target);

// The style change of a text action (MoveWithLine ... CenterVertically) made on `s`; false for the
// other actions, which leave the style alone.
bool gripActionStyle(GripAction a, DimStyle& s);

// Command line that does the action from the UI: `GRIPEDIT x,y Keyword` (the user is then asked
// for the point), or the dimension command for Continue / Baseline. Empty when not applicable.
std::string gripActionCommand(const Document& doc, const Grip& grip, GripAction action);

}  // namespace tbcad::transform
