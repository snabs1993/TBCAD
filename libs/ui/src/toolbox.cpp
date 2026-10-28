#include "tbcad/ui/toolbox.h"

namespace tbcad::ui {

namespace {

using G = ToolGroup;
using T = ToolDef;
using V = ToolVariant;

std::vector<ToolTab> build() {
    std::vector<ToolTab> tabs;

    tabs.push_back(
        {"draw",
         "Draw",
         {G{"Lines",
            {T{"Line", "LINE", "line", {}}, T{"Polyline", "POLYLINE", "polyline", {}},
             T{"Ray", "RAY", "ray", {}}, T{"Construction", "XLINE", "xline", {}},
             T{"Multiline", "MLINE", "mline", {}}, T{"Sketch", "SKETCH", "sketch", {}}}},
          G{"Curves",
            {T{"Circle",
               "CIRCLE",
               "circle",
               {V{"Centre, radius", "CIRCLE"}, V{"Two points", "CIRCLE 2P"},
                V{"Three points", "CIRCLE 3P"}}},
             T{"Arc", "ARC", "arc", {}}, T{"Ellipse", "ELLIPSE", "ellipse", {}},
             T{"Donut", "DONUT", "donut", {}}, T{"Cloud", "REVCLOUD", "revcloud", {}},
             T{"Spline", "SPLINE", "spline", {}}}},
          G{"Shapes",
            {T{"Rectangle", "RECTANGLE", "rectangle", {}}, T{"Polygon", "POLYGON", "polygon", {}},
             T{"Point", "POINT", "point", {}}, T{"Divide", "DIVIDE", "divide", {}},
             T{"Measure", "MEASURE", "measure", {}}, T{"Solid", "SOLID", "solid", {}},
             T{"Wipeout", "WIPEOUT", "wipeout", {}}}},
          G{"Fill & area",
            {T{"Hatch", "HATCH", "hatch", {}}, T{"Gradient", "GRADIENT", "gradient", {}},
             T{"Boundary", "BOUNDARY", "boundary", {}}, T{"Region", "REGION", "region", {}}}},
          G{"Table",
            {T{"Table", "TABLE", "table", {}}, T{"Import", "TABLEIMPORT", "table", {}}}}}});

    tabs.push_back(
        {"modify",
         "Modify",
         {G{"Move & copy",
            {T{"Move", "MOVE", "move", {}}, T{"Copy", "COPY", "copy", {}},
             T{"Rotate", "ROTATE", "rotate", {}}, T{"Scale", "SCALE", "scale", {}},
             T{"Mirror", "MIRROR", "mirror", {}}, T{"Align", "ALIGN", "align", {}},
             T{"Stretch", "STRETCH", "stretch", {}},
             T{"Array",
               "ARRAY",
               "array",
               {V{"Rectangular", "ARRAY Rectangular"}, V{"Polar", "ARRAY Polar"},
                V{"Along path", "ARRAY Path"}}}}},
          G{"Cut & join",
            {T{"Trim", "TRIM", "trim", {}}, T{"Extend", "EXTEND", "extend", {}},
             T{"Break", "BREAK", "break", {}},
             T{"Break at point", "BREAKATPOINT", "breakatpoint", {}}, T{"Join", "JOIN", "join", {}},
             T{"Fillet", "FILLET", "fillet", {}}, T{"Chamfer", "CHAMFER", "chamfer", {}},
             T{"Blend", "BLEND", "blend", {}}, T{"Lengthen", "LENGTHEN", "lengthen", {}},
             T{"Offset", "OFFSET", "offset", {}}}},
          G{"Objects",
            {T{"Erase", "ERASE", "erase", {}}, T{"Explode", "EXPLODE", "explode", {}},
             T{"Reverse", "REVERSE", "reverse", {}}, T{"Overkill", "OVERKILL", "overkill", {}},
             T{"Polyline edit", "PEDIT", "editobject", {}},
             T{"Spline edit", "SPLINEDIT", "editobject", {}},
             T{"Hatch edit", "HATCHEDIT", "editobject", {}}}},
          G{"Manage",
            {T{"Match", "MATCHPROP", "matchprop", {}}, T{"Group", "GROUP", "group", {}},
             T{"Ungroup", "UNGROUP", "ungroup", {}}, T{"Draw order", "DRAWORDER", "draworder", {}},
             T{"By layer", "SETBYLAYER", "bylayer", {}},
             T{"To layer", "COPYTOLAYER", "tolayer", {}}}},
          G{"Clipboard",
            {T{"Copy", "COPYCLIP", "copy", {}}, T{"Cut", "CUTCLIP", "cut", {}},
             T{"Paste", "PASTECLIP", "paste", {}}, T{"As block", "PASTEBLOCK", "paste", {}}}}}});

    tabs.push_back(
        {"annotate",
         "Annotate",
         {G{"Text",
            {T{"Text", "TEXT", "text", {}}, T{"Multiline text", "MTEXT", "mtext", {}},
             T{"Field", "FIELD", "field", {}}, T{"Edit text", "TEXTEDIT", "text", {}},
             T{"Scale text", "SCALETEXT", "scale", {}}, T{"Justify", "JUSTIFYTEXT", "text", {}},
             T{"Find", "FIND", "find", {}}}},
          G{"Dimensions",
            {T{"Linear", "DIMLINEAR", "dimension", {}},
             T{"Aligned", "DIMALIGNED", "dimaligned", {}},
             T{"Angle", "DIMANGULAR", "dimangular", {}}, T{"Radius", "DIMRADIUS", "dimradius", {}},
             T{"Diameter", "DIMDIAMETER", "dimdiameter", {}}, T{"Leader", "LEADER", "leader", {}},
             T{"Tolerance", "TOLERANCE", "tolerance", {}},
             T{"Centre mark", "CENTERMARK", "centermark", {}},
             T{"Centre line", "CENTERLINE", "centermark", {}}}},
          G{"Styles",
            {T{"Text style", "STYLE", "text", {}}, T{"Dim styles", "DIMSTYLES", "dimension", {}},
             T{"Leader style", "MLEADERSTYLE", "leader", {}},
             T{"Table style", "TABLESTYLE", "table", {}},
             T{"Scale", "CANNOSCALE", "scale", {}}}}}});

    tabs.push_back(
        {"blocks",
         "Blocks",
         {G{"Blocks",
            {T{"Make block", "BLOCK", "block", {}}, T{"Insert", "INSERT", "insert", {}},
             T{"Palette", "BLOCKSPALETTE", "insert", {}},
             T{"Define attribute", "ATTDEF", "attribute", {}},
             T{"Edit attribute", "ATTEDIT", "attribute", {}},
             T{"Block editor", "BEDIT", "block", {}}}},
          G{"References",
            {T{"Attach", "XATTACH", "xref", {}}, T{"Manage", "EXTERNALREFERENCES", "xref", {}},
             T{"Reload", "XRELOAD", "xref", {}}, T{"Clip", "XCLIP", "xref", {}},
             T{"Content", "CONTENT", "insert", {}}}}}});

    tabs.push_back(
        {"inspect",
         "Inspect",
         {G{"Measure",
            {T{"Distance", "DIST", "distance", {}}, T{"Area", "AREA", "area", {}},
             T{"Angle", "ANGLE", "angle", {}}, T{"Point ID", "ID", "id", {}},
             T{"List", "LIST", "list", {}}, T{"Quick", "QUICKMEASURE", "distance", {}},
             T{"Calculator", "CAL", "calc", {}}, T{"Area table", "AREATABLE", "area", {}},
             T{"Block count", "BLOCKCOUNT", "block", {}}}},
          G{"Check & repair",
            {T{"Audit", "AUDIT", "audit", {}}, T{"Purge", "PURGE", "purge", {}},
             T{"Recover", "RECOVER", "audit", {}}, T{"Standards", "CHECKSTANDARDS", "audit", {}},
             T{"Quick select", "QSELECT", "find", {}}, T{"Filter", "FILTER", "find", {}}}},
          G{"Drawing aids",
            {T{"Snap", "SNAP", "snap", {}}, T{"Grid", "GRID", "grid", {}},
             T{"Ortho", "ORTHO", "ortho", {}}, T{"Polar", "POLAR", "polar", {}},
             T{"Object snap", "OSNAP", "osnap", {}}}}}});

    tabs.push_back(
        {"output",
         "Output",
         {G{"Print",
            {T{"Plot", "PLOTPANEL", "plot", {}}, T{"Preview", "PREVIEW", "plot", {}},
             T{"Page setup", "PAGESETUPPANEL", "plot", {}},
             T{"Plot style", "PLOTSTYLE", "plot", {}}, T{"Publish", "PUBLISH", "plot", {}}}},
          G{"Export",
            {T{"PDF", "EXPORTPDF", "pdf", {}}, T{"PNG", "EXPORTPNG", "pdf", {}},
             T{"JPG", "EXPORTJPG", "pdf", {}}, T{"SVG", "EXPORTSVG", "pdf", {}},
             T{"Properties", "DWGPROPS", "list", {}}}}}});
    return tabs;
}

}  // namespace

const std::vector<ToolTab>& toolboxTabs() {
    static const std::vector<ToolTab> tabs = build();
    return tabs;
}

const ToolDef* findTool(std::string_view command) {
    for (const ToolTab& tab : toolboxTabs())
        for (const ToolGroup& g : tab.groups)
            for (const ToolDef& t : g.tools)
                if (t.command == command)
                    return &t;
    return nullptr;
}

std::vector<std::string> defaultQuickRail() {
    return {"LINE", "POLYLINE", "CIRCLE", "MOVE", "TRIM", "ERASE"};
}

}  // namespace tbcad::ui
