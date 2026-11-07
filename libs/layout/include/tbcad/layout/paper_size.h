// Paper sizes of annotation: how tall text and dimension text come out on the printed sheet, so
// people can see that a note is 2.5 mm on A1 and catch notes left at an old scale. Each object
// belongs to one "paper place": the plot frame that holds it, else a layout viewport that shows
// it, else an annotation zone, else the drawing's annotation scale. Objects on a layout sheet are
// already in paper millimetres.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layout {

struct PaperPlace {
    enum class Kind { Frame, Viewport, Zone, AnnotationScale, Sheet };
    Kind kind = Kind::AnnotationScale;
    double scaleN = 1;  // 1:N, 1 mm on paper = N drawing units
    // The scale annotative objects are drawn at there (0 = the drawing's annotation scale, as a
    // fit-to-paper frame plots them).
    double annoOverride = 0;
    std::string name{};       // frame, layout or zone name
    int viewport = 0;         // Kind::Viewport: the viewport id in layout `name`
    std::string paperName{};  // "A1" ... for frames and sheets; empty otherwise
};

// Where a model space point is printed (see above). Frames and zones: the smallest that holds
// the point; viewports: the first layout (in tab order) with an "on" viewport showing it.
PaperPlace paperPlaceAt(const Document& doc, Vec2 modelPoint);
// The place of an entity: by its anchor in model space, Kind::Sheet on a layout sheet. None for
// objects inside block definitions and for kinds without an anchor.
std::optional<PaperPlace> paperPlaceOf(const Document& doc, EntityId id);

// The text height of a text, multiline text, dimension, leader or multileader in drawing units as
// it is drawn at `place` (annotative objects at the place's scale). None for other kinds.
std::optional<double> drawnTextHeight(const Document& doc, const Entity& e,
                                      const PaperPlace& place);
// The same on paper, in millimetres.
std::optional<double> paperTextHeightMm(const Document& doc, const Entity& e,
                                        const PaperPlace& place);

// "1:100, frame Sheet 3 (A1)", "1:50, Layout1 viewport 2 (A3)", "1:20, zone Detail",
// "1:100, annotation scale", "sheet Layout1 (A1)".
std::string describePaperPlace(const PaperPlace& place);

// Paper sizes of the text-bearing objects among `ids`, for the Properties panel.
struct PaperSizeSummary {
    std::size_t count = 0;  // objects with a paper size
    double minMm = 0, maxMm = 0;
    // The place when every object shares one (same kind, name, viewport and scale).
    std::optional<PaperPlace> place{};
};
PaperSizeSummary paperSizeSummary(const Document& doc, const std::vector<EntityId>& ids);
// "2.5 mm" or "1.8 - 5 mm" (two decimals at most, trailing zeros dropped).
std::string formatPaperMm(double minMm, double maxMm);

}  // namespace tbcad::layout

namespace tbcad::layout {

// ---- paper check (PAPERCHECK): text and dimension text that will not print well -------------

struct PaperCheckOptions {
    double minMm = 1.8;  // below: too small to read
    double maxMm = 10;   // above: too big for a note
    double toleranceMm = 0.05;
    std::vector<double> standardMm{1.8, 2.5, 3.5, 5, 7, 10};  // heights people use on paper
};

enum class PaperIssue {
    Ok,
    OtherScale,  // reads at a standard height at another common scale: left from a scale change
    TooSmall,
    TooBig,
    OffStandard,  // readable, but not one of the standard heights
};
// "OK", "Made for another scale", "Too small", "Too big", "Not a standard height".
const char* paperIssueLabel(PaperIssue issue);

struct PaperCheckItem {
    EntityId id{};
    PaperPlace place{};
    double mm = 0;  // height on paper
    PaperIssue issue = PaperIssue::Ok;
    double madeForN = 0;   // OtherScale: the 1:N it reads right at
    double suggestMm = 0;  // the standard height to fit it to
};

// The text-bearing objects of model space and of every layout sheet, in that order.
std::vector<EntityId> paperObjects(const Document& doc);
// Checks `ids` (empty: paperObjects). Objects without a paper size are left out.
std::vector<PaperCheckItem> paperCheck(const Document& doc, const std::vector<EntityId>& ids = {},
                                       const PaperCheckOptions& options = {});
// Sets each item to its suggested paper height: text height for text and multiline text, a
// dimension override style (overall scale) for dimensions and leaders, the note height for a
// multileader. Items already OK are skipped. Returns how many objects changed.
Result<std::size_t> fitToPaper(Transaction& tx, const std::vector<PaperCheckItem>& items);

}  // namespace tbcad::layout
