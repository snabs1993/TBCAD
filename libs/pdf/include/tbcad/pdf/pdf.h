// Low-level PDF 1.4 writer for vector drawings (W1-3).
//
// All page content is expressed in millimetres with the origin at the
// bottom-left of the page. The writer installs a single page-level `cm`
// (1 mm = 72/25.4 pt) so callers never convert units themselves.
#pragma once

#include <cstddef>
#include <deque>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // Raw (uncompressed) content stream, mainly for tests.
    const std::string& content() const { return content_; }

private:
    friend class PdfDocument;
    Page(double widthMm, double heightMm) : widthMm_(widthMm), heightMm_(heightMm) {}
    void op(std::string_view text);
    void nums(std::initializer_list<double> values, std::string_view opName);

    double widthMm_;
    double heightMm_;
    std::string content_;
    bool hasCurrentPoint_ = false;
};

class PdfDocument {
public:
    void setInfo(std::string_view title, std::string_view creator = "TBCAD");  // UTF-8
    // Optional PDF date string such as "D:20260101120000Z". Omitted by default so
    // output is deterministic.
    void setCreationDate(std::string_view pdfDate) { creationDate_ = pdfDate; }
    void setCompression(bool enabled) { compress_ = enabled; }

    Page& addPage(double widthMm, double heightMm);
    size_t pageCount() const { return pages_.size(); }

    Result<void> write(std::ostream& out) const;
    Result<void> writeFile(const std::string& path) const;  // UTF-8 path

private:
    std::string title_;
    std::string creator_ = "TBCAD";
    std::string creationDate_;
    bool compress_ = true;
    std::deque<Page> pages_;  // deque: addPage never invalidates earlier references
};

}  // namespace tbcad::pdf
