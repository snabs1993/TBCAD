// Low-level PDF 1.4 writer for vector drawings (W1-3).
//
// All page content is expressed in millimetres with the origin at the
// bottom-left of the page. The writer installs a single page-level `cm`
// (1 mm = 72/25.4 pt) so callers never convert units themselves.
#pragma once

#include <cstddef>
#include <deque>
#include <initializer_list>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/core/image.h"
#include "tbcad/core/result.h"

namespace tbcad::pdf {

enum class FillRule { NonZero, EvenOdd };
enum class LineCap { Butt = 0, Round = 1, Square = 2 };
enum class LineJoin { Miter = 0, Round = 1, Bevel = 2 };

// "Default" lineweight maps to this, never to 0 (0 means "thinnest the device can render").
inline constexpr double kDefaultLineWidthMm = 0.25;

// Fixed-precision number for PDF output: at most 4 decimals, trailing zeros
// trimmed, never an exponent, non-finite values written as 0. `decimals` is only
// raised for matrix entries (rotation/scale terms).
std::string formatNumber(double value, int decimals = 4);

class PdfDocument;

// A TrueType font (glyph outlines in a 'glyf' table) to embed: only the glyphs the pages use are
// kept, and they are addressed by glyph id, so text shaped elsewhere (Thai marks, ligatures) is
// written exactly as laid out.
struct EmbeddedFont {
    std::shared_ptr<const std::vector<unsigned char>> file{};  // the .ttf or .ttc
    int faceIndex = 0;
    std::string postscriptName{};  // e.g. "ArialMT"; empty = from the font. Subsets get "ABCDEF+"
};

// True when `file` (face `faceIndex`) can be embedded as an EmbeddedFont.
bool canEmbedTrueType(const std::vector<unsigned char>& file, int faceIndex);

class Page {
public:
    double widthMm() const { return widthMm_; }
    double heightMm() const { return heightMm_; }

    // Graphics state.
    void save();
    void restore();
    void transform(double a, double b, double c, double d, double e, double f);
    void setStrokeRGB(double r, double g, double b);  // components 0..1
    void setFillRGB(double r, double g, double b);
    void setLineWidthMm(double widthMm);
    void setDashMm(const std::vector<double>& pattern, double phase = 0.0);  // empty = solid
    void setLineCap(LineCap cap);
    void setLineJoin(LineJoin join);
    void setMiterLimit(double limit);

    // Path construction (mm).
    void moveTo(double x, double y);
    void lineTo(double x, double y);
    void curveTo(double x1, double y1, double x2, double y2, double x3, double y3);
    void closePath();
    void rect(double x, double y, double w, double h);
    // Circular arc about (cx, cy) from startRad to endRad (counter-clockwise when
    // ccw, else clockwise), as cubic Béziers of at most 90° each. A line is drawn
    // from the current point to the arc start; with no current point the arc
    // begins a new subpath.
    void arcTo(double cx, double cy, double radius, double startRad, double endRad,
               bool ccw = true);

    // Painting.
    void stroke();
    void fill(FillRule rule = FillRule::NonZero);
    void fillStroke(FillRule rule = FillRule::NonZero);
    void clipRect(double x, double y, double w, double h);
    // Clips to a closed polygon (even-odd); `pts` holds x, y pairs in mm.
    void clipPolygon(const std::vector<std::pair<double, double>>& pts);

    // A picture (RGBA) whose lower-left pixel corner is at `bl`, with `br` and `tl` the other two
    // corners it is stretched to (mm). `opacity` 0..1. The pixels are written once per document
    // even when several pages or places use the same data.
    void drawImage(std::shared_ptr<const ImageData> image, double blx, double bly, double brx,
                   double bry, double tlx, double tly, double opacity = 1.0);

    // One glyph of a font made with PdfDocument::addFont, filled with the current fill colour.
    // The matrix (a b c d e f) maps the glyph's em square (1 = one em, y up) to the page (mm).
    // `text` is the character(s) the glyph stands for, used for copying and searching (empty for
    // the second and later glyphs of one character). Consecutive glyphs share one text object;
    // any other operation ends it.
    void showGlyph(int font, std::uint16_t glyphId, std::u32string_view text, double a, double b,
                   double c, double d, double e, double f);

    // Everything drawn between beginLayer and endLayer belongs to an optional-content group made
    // with PdfDocument::addLayer (a layer the viewer can switch off). Not nested.
    void beginLayer(int layer);
    void endLayer();

    // Raw (uncompressed) content stream, mainly for tests.
    std::string content() const { return inText_ ? content_ + "ET\n" : content_; }

private:
    friend class PdfDocument;
    Page(double widthMm, double heightMm) : widthMm_(widthMm), heightMm_(heightMm) {}
    void op(std::string_view text);
    void nums(std::initializer_list<double> values, std::string_view opName);
    void endText();

    double widthMm_;
    double heightMm_;
    std::string content_;
    bool hasCurrentPoint_ = false;
    struct Pic {
        std::shared_ptr<const ImageData> data{};
        double opacity = 1;
    };
    std::vector<Pic> pics_{};        // /Im<i> and /Gs<i> of the page resources
    std::vector<int> layersUsed_{};  // /L<i> of the page resources
    bool inText_ = false;
    int textFont_ = -1;              // font selected in the open text object
    std::set<int> fontsUsed_{};      // /F<i> of the page resources
    // Per font: glyph id -> the text it stands for (the first one seen), for ToUnicode.
    std::map<int, std::map<std::uint16_t, std::u32string>> glyphsUsed_{};
};

class PdfDocument {
public:
    void setInfo(std::string_view title, std::string_view creator = "TBCAD");  // UTF-8
    // Optional PDF date string such as "D:20260101120000Z". Omitted by default so
    // output is deterministic.
    void setCreationDate(std::string_view pdfDate) { creationDate_ = pdfDate; }
    void setCompression(bool enabled) { compress_ = enabled; }

    // A layer (optional-content group) shown in the viewer's layer list, in the order added.
    // Returns its number for Page::beginLayer.
    int addLayer(std::string_view name);

    // A font for Page::showGlyph; returns its number. Check canEmbedTrueType first: a font that
    // cannot be embedded makes write() fail.
    int addFont(EmbeddedFont font);

    Page& addPage(double widthMm, double heightMm);
    size_t pageCount() const { return pages_.size(); }

    Result<void> write(std::ostream& out) const;
    Result<void> writeFile(const std::string& path) const;  // UTF-8 path

private:
    std::string title_;
    std::string creator_ = "TBCAD";
    std::string creationDate_;
    bool compress_ = true;
    std::vector<std::string> layerNames_;
    std::vector<EmbeddedFont> fonts_;
    std::deque<Page> pages_;  // deque: addPage never invalidates earlier references
};

}  // namespace tbcad::pdf
