#pragma once

#include <imgui.h>

#include <string_view>

namespace tbcad::app {

// The colour family of an icon: the accent dot or stroke follows it, so the toolbox can be scanned
// by colour (draw = blue, modify = orange, annotate = green ...).
enum class IconGroup { General, Draw, Modify, Annotate, Block, Inquire, Output, Toggle };

#define TBCAD_ICON_LIST(X) \
    X(New, General) \
    X(Open, General) \
    X(Save, General) \
    X(Undo, General) \
    X(Redo, General) \
    X(Line, Draw) \
    X(Polyline, Draw) \
    X(Circle, Draw) \
    X(Arc, Draw) \
    X(Rectangle, Draw) \
    X(Hatch, Draw) \
    X(Text, Annotate) \
    X(Move, Modify) \
    X(Copy, Modify) \
    X(Rotate, Modify) \
    X(Scale, Modify) \
    X(Mirror, Modify) \
    X(Stretch, Modify) \
    X(Trim, Modify) \
    X(Offset, Modify) \
    X(Fillet, Modify) \
    X(Erase, Modify) \
    X(ZoomExtents, General) \
    X(Ellipse, Draw) \
    X(Spline, Draw) \
    X(Polygon, Draw) \
    X(Point, Draw) \
    X(Dimension, Annotate) \
    X(Block, Block) \
    X(Insert, Block) \
    X(Array, Modify) \
    X(Explode, Modify) \
    X(Chamfer, Modify) \
    X(Extend, Modify) \
    X(Join, Modify) \
    X(Ray, Draw) \
    X(Xline, Draw) \
    X(Mline, Draw) \
    X(Sketch, Draw) \
    X(Donut, Draw) \
    X(Revcloud, Draw) \
    X(Divide, Draw) \
    X(Measure, Draw) \
    X(Wipeout, Draw) \
    X(Solid, Draw) \
    X(Boundary, Draw) \
    X(Region, Draw) \
    X(Table, Annotate) \
    X(Align, Modify) \
    X(Break, Modify) \
    X(BreakAtPoint, Modify) \
    X(Blend, Modify) \
    X(Lengthen, Modify) \
    X(Extrim, Modify) \
    X(Reverse, Modify) \
    X(Overkill, Modify) \
    X(EditObject, Modify) \
    X(MatchProp, Modify) \
    X(Group, Block) \
    X(Ungroup, Block) \
    X(DrawOrder, Modify) \
    X(Union, Modify) \
    X(Subtract, Modify) \
    X(Intersect, Modify) \
    X(Paste, General) \
    X(Cut, General) \
    X(MText, Annotate) \
    X(Field, Annotate) \
    X(DimAligned, Annotate) \
    X(DimAngular, Annotate) \
    X(DimRadius, Annotate) \
    X(DimDiameter, Annotate) \
    X(Leader, Annotate) \
    X(CenterMark, Annotate) \
    X(Attribute, Block) \
    X(Xref, Block) \
    X(Distance, Inquire) \
    X(Area, Inquire) \
    X(Angle, Inquire) \
    X(Id, Inquire) \
    X(List, Inquire) \
    X(Calc, Inquire) \
    X(Audit, Inquire) \
    X(Purge, Inquire) \
    X(Plot, Output) \
    X(Pdf, Output) \
    X(Layers, General) \
    X(Levels, General) \
    X(Snap, Toggle) \
    X(Grid, Toggle) \
    X(Ortho, Toggle) \
    X(Polar, Toggle) \
    X(Osnap, Toggle) \
    X(Gradient, Draw) \
    X(ByLayer, Modify) \
    X(ToLayer, Modify) \
    X(Find, Inquire) \
    X(Filter, Inquire) \
    X(Tolerance, Annotate)

// Tool-strip icons, drawn with line primitives on a 20 x 20 grid: single colour, 1.5 px stroke at
// 100 % scale, so they follow the theme and every DPI without image files.
enum class Icon {
#define X(name, group) name,
    TBCAD_ICON_LIST(X)
#undef X
        Count
};

// Short English name of an icon ("Line", "DimAligned"), and the group it belongs to.
const char* iconName(Icon icon);
IconGroup iconGroup(Icon icon);
// Accent colour of a group (a stable colour that reads on dark and light themes).
ImU32 iconAccent(IconGroup group);

// Draws `icon` centred in the square [min, min + size). The second colour (when not 0) is used for
// the one accent part each icon may have; 0 draws everything in `color`.
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 color, ImU32 accent = 0);

// The icon with this id (the ids the Toolbox table uses, lower case); false when there is none
// yet, and the caller draws the tool's initials instead.
bool iconFromId(std::string_view id, Icon& out);

// An icon button of the given square size (frame included); true when clicked.
bool iconButton(const char* id, Icon icon, float size, bool enabled = true);

}  // namespace tbcad::app
