// Paper-space layouts: a sheet of paper with its own drawing objects (title block, notes) and any
// number of viewports that show model space at a scale, with layers that can be frozen per
// viewport.
//
// Storage (no changes to core or the file format): the layout table is one text value in
// Header::custom["tbcad.layouts"], so it is saved, loaded and undone with the drawing. The objects
// that live on the sheet belong to a hidden anonymous block per layout ("*LAYOUT-<uid>"); their
// coordinates are millimetres on the sheet with the origin at the lower-left corner.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"

namespace tbcad::layout {

inline constexpr const char* kHeaderKey = "tbcad.layouts";
// Layers that are frozen in every viewport added from now on (VPLAYER New freeze, the "frozen in
// new viewports" flag of a DXF layer): a comma list of layer ids.
inline constexpr const char* kNewViewportFrozenKey = "tbcad.vpnewfrozen";

// How one layer looks on one sheet, apart from the drawing and from other sheets (SHEETLAYER, the
// "This sheet" mode of the Layers panel). Each value left empty follows the drawing's layer.
// It acts on the model shown in the sheet's viewports, not on the sheet's own objects.
struct LayerOverride {
    LayerId layer{};
    std::optional<bool> on{};      // shown or hidden on this sheet (true also shows a frozen layer)
    std::optional<Color> color{};  // Index or Rgb
    std::optional<LinetypeId> linetype{};    // a real linetype
    std::optional<LineWeight> lineweight{};  // Default or a value
    std::optional<int> transparency{};       // percent, 0 to 90
    std::optional<bool> plot{};

    bool empty() const {
        return !on && !color && !linetype && !lineweight && !transparency && !plot;
    }
    friend bool operator==(const LayerOverride&, const LayerOverride&) = default;
};

struct Viewport {
    int id = 0;     // unique within the layout, starts at 1
    BBox2 paper{};  // rectangle on the sheet (mm); the bounding box when the viewport is clipped
    // VPCLIP: the outline of a non-rectangular viewport on the sheet (mm), three or more points;
    // empty = the rectangle. `paper` is its bounding box, so scale and view centre keep meaning.
    std::vector<Vec2> clip{};
    Vec2 center{};      // model point shown at the middle of the viewport
    double scaleN = 1;  // 1:N, 1 mm on the sheet = N model units
    bool on = true;
    std::vector<LayerId> frozen{};  // frozen in this viewport (their sub-layers too)
    LevelId level{};                // shows only this level when set (0 = every visible level)
    bool locked = false;            // scale and view centre cannot be changed (VPLOCK)
    bool border = true;     // the rectangle is drawn and plotted (false: its layer does not plot)
    LayerId borderLayer{};  // MVIEW Layer: the border takes this layer's colour and goes with its
                            // on / frozen / plot state (null = the border is always drawn)
    // Overrides of this viewport alone (read from other programs' files, which keep them per
    // viewport); they win over the sheet's.
    std::vector<LayerOverride> overrides{};
};

// True when the sheet point is inside the viewport's outline.
bool viewportContains(const Viewport& vp, Vec2 sheetPoint);
// The outline: the clip polygon, or the four corners of the rectangle (counter-clockwise).
std::vector<Vec2> viewportOutline(const Viewport& vp);

struct Layout {
    int uid = 0;
    std::string name{};
    std::string paperName{};  // "A3", ... or "Custom"
    bool landscape = true;
    double widthMm = 0, heightMm = 0;  // as oriented
    BlockId content{};                 // block holding the sheet's own objects
    std::string pageSetup{};           // name of the assigned page setup (pagesetup.h), or empty
    std::vector<Viewport> viewports{};
    std::vector<LayerOverride> layerOverrides{};  // this sheet's, one per layer

    const Viewport* findViewport(int id) const;
    const LayerOverride* findOverride(LayerId layer) const;
};

// All layouts, in creation order. Unreadable lines are skipped.
std::vector<Layout> layouts(const Document& doc);
// By name, ASCII case-insensitive.
std::optional<Layout> findLayout(const Document& doc, std::string_view name);

// Names: 1-64 characters, no control characters, unique ignoring case.
Result<void> validateLayoutName(const Document& doc, std::string_view name,
                                std::string_view ignore = {});

Result<void> createLayout(Transaction& tx, const std::string& name,
                          const std::string& paperName = "A3", bool landscape = true);
Result<void> deleteLayout(Transaction& tx, const std::string& name);
// Copies a layout under a new name: the paper, the page setup, the viewports (same ids, scales,
// frozen layers) and every object on the sheet.
Result<void> copyLayout(Transaction& tx, const std::string& name, const std::string& newName);
Result<void> renameLayout(Transaction& tx, const std::string& name, const std::string& newName);
// Puts the layout at a 0-based place in the tab order (past the end means last).
Result<void> moveLayout(Transaction& tx, const std::string& name, std::size_t position);
// Changes the sheet; viewports and objects stay where they are (in millimetres).
// Sets only the name of the page setup the layout uses (see pagesetup.h; use assignPageSetup).
Result<void> setLayoutPageSetupName(Transaction& tx, const std::string& name,
                                    const std::string& pageSetup);
Result<void> setLayoutPaper(Transaction& tx, const std::string& name, const std::string& paperName,
                            bool landscape, double customWidthMm = 0, double customHeightMm = 0);

// Returns the new viewport's id.
Result<int> addViewport(Transaction& tx, const std::string& layout, const BBox2& paperRect,
                        Vec2 modelCenter, double scaleN);
Result<void> removeViewport(Transaction& tx, const std::string& layout, int id);
Result<void> setViewportScale(Transaction& tx, const std::string& layout, int id, double scaleN);
// A locked viewport refuses setViewportScale and setViewportCenter (VPLOCK).
// The layer the border of viewport `id` belongs to (a null layer = none).
Result<void> setViewportBorderLayer(Transaction& tx, const std::string& layoutName, int id,
                                    LayerId layer);
Result<void> setViewportLocked(Transaction& tx, const std::string& layoutName, int id, bool locked);
Result<void> setViewportCenter(Transaction& tx, const std::string& layout, int id, Vec2 center);
// Moves or resizes the rectangle on the sheet; what it shows keeps its scale and centre.
Result<void> setViewportRect(Transaction& tx, const std::string& layout, int id,
                             const BBox2& paperRect);
// Gives the viewport a polygonal outline (VPCLIP); fewer than three points restore the rectangle.
// The rectangle becomes the bounding box of the polygon. A locked viewport refuses it.
Result<void> setViewportClip(Transaction& tx, const std::string& layout, int id,
                             const std::vector<Vec2>& outline);
Result<void> setViewportOn(Transaction& tx, const std::string& layout, int id, bool on);
// Freezes or thaws one layer in one viewport only; the layer table is not touched.
Result<void> setViewportLayerFrozen(Transaction& tx, const std::string& layout, int id,
                                    LayerId layer, bool frozen);

// Shows one level only in the viewport (an empty id shows every visible level again).
Result<void> setViewportLevel(Transaction& tx, const std::string& layout, int id, LevelId level);

// The layers frozen in new viewports, and the switch for one layer.
std::vector<LayerId> layersFrozenInNewViewports(const Document& doc);
Result<void> setLayerFrozenInNewViewports(const Document& doc, Transaction& tx, LayerId layer,
                                          bool frozen);

// True when the layer, or one of its parents, is frozen in the viewport.
bool frozenInViewport(const Document& doc, const Viewport& vp, LayerId layer);

// ---- per-sheet layer overrides --------------------------------------------------------------

// Sets the sheet's override of `o.layer` (replacing the old one); an empty override removes it.
Result<void> setSheetLayerOverride(Transaction& tx, const std::string& layout,
                                   const LayerOverride& o);
// Removes every override of the sheet (and of its viewports); `layer` set: only that layer's.
Result<void> clearSheetLayerOverrides(Transaction& tx, const std::string& layout,
                                      LayerId layer = {});
// The layer as it shows in the viewport of the layout: the drawing's layer with the sheet's
// override and then the viewport's own applied. A hidden parent (on = false) hides its sub-layers.
EffectiveLayer sheetLayer(const Document& doc, const Layout& layout, const Viewport* vp,
                          LayerId layer);
// Applies the overrides to a layer already resolved for the drawing (what sheetLayer does).
void applySheetOverrides(const Document& doc, const Layout& layout, const Viewport* vp,
                         EffectiveLayer& layer);
// True when the layer does not show in the viewport: frozen there, or hidden on the sheet.
bool hiddenInViewport(const Document& doc, const Layout& layout, const Viewport& vp, LayerId layer);

// Text form of one override's values (TAB separated: on, colour, linetype id, lineweight,
// transparency, plot; an empty field is not overridden), as stored and as written to DXF with the
// linetype id replaced by its name by the caller.
std::string overrideFields(const LayerOverride& o);
LayerOverride parseOverrideFields(const std::vector<std::string>& fields, std::size_t first);

// Adds a framed title block with the standard fields (project, drawing, scale, date, sheet) to
// the layout's sheet: a border and text objects in the lower right corner.
struct TitleBlockFields {
    std::string project{}, drawing{}, scale{}, date{}, sheet{};
};
Result<void> addTitleBlock(Transaction& tx, const std::string& layout,
                           const TitleBlockFields& fields);

// ---- plotting -------------------------------------------------------------------------------

struct LayoutPlotOptions {
    bool viewportBorders = true;  // draw each viewport's rectangle
};
// The sheet at 1:1: the layout's own objects, then each viewport's model-space view clipped to the
// viewport. `base` supplies lineweights, colour mode, chord tolerance and hairline width; its
// paper, area, scale and margins are ignored.
Result<exportlib::PlotSheet> buildLayoutSheet(const Document& doc, const std::string& layout,
                                              const exportlib::PlotSettings& base = {},
                                              const LayoutPlotOptions& options = {});

}  // namespace tbcad::layout
