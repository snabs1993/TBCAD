// Printing a plotted sheet. `drawSheet` turns the paper-space primitives of a PlotSheet into
// device calls on a Canvas (pure, tested everywhere); the Windows backend implements the Canvas
// with GDI and drives the system print dialog. Other systems get a stub that reports an error.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"

namespace tbcad::print {

struct PrinterInfo {
    std::string name{};
    bool isDefault = false;
};

struct PaperInfo {
    std::string name{};
    double widthMm = 0, heightMm = 0;  // as the driver reports it, width <= height
};

// True when this build can talk to printers.
bool available();
std::vector<PrinterInfo> listPrinters();
// The paper sizes the driver offers (empty when unknown).
std::vector<PaperInfo> printerPapers(const std::string& printer);

struct PrintJob {
    std::string printer{};  // empty = the default printer
    std::string title = "TBCAD drawing";
    bool showDialog = false;  // let the user pick the printer and copies in the system dialog
    int copies = 1;
};

// One page per sheet. The paper size and orientation come from each sheet's layout.
Result<void> printSheets(const std::vector<exportlib::PlotSheet>& sheets, const PrintJob& job);
inline Result<void> printSheet(const exportlib::PlotSheet& sheet, const PrintJob& job) {
    return printSheets({sheet}, job);
}

// ---- Device drawing (shared by every backend) ---------------------------------------------------

// Where the printable area sits on the device, in device pixels.
struct DeviceMetrics {
    double dpiX = 600, dpiY = 600;
    // The physical page, and the offset of the printable area from its top-left corner.
    double pageWidthPx = 0, pageHeightPx = 0;
    double offsetXPx = 0, offsetYPx = 0;
};

// A device that draws in pixels, origin at the top-left of the printable area, y downwards.
class Canvas {
public:
    virtual ~Canvas() = default;
    virtual void polyline(const std::vector<Vec2>& px, bool closed, Rgba color, double widthPx,
                          const std::vector<double>& dashPx) = 0;
    // Even-odd fill of several rings.
    virtual void fill(const std::vector<std::vector<Vec2>>& rings, Rgba color) = 0;
    virtual void dot(Vec2 px, Rgba color, double diameterPx) = 0;
    virtual void pushClip(const BBox2& rectPx) = 0;
    virtual void popClip() = 0;
    // A picture whose pixel corners land on `corners` (bottom-left, bottom-right, top-right,
    // top-left, device pixels); `opacity` 0..1 over white paper. Devices that cannot draw
    // pictures leave it out.
    virtual void image(const ImageData& data, const std::array<Vec2, 4>& corners, double opacity) {
        (void)data;
        (void)corners;
        (void)opacity;
    }
};

// Paper millimetres (lower-left origin) to device pixels.
Vec2 toDevice(Vec2 paperMm, const DeviceMetrics& m);

// Draws every primitive of the sheet. Arcs are flattened to polylines.
void drawSheet(const exportlib::PlotSheet& sheet, const DeviceMetrics& metrics, Canvas& canvas);

}  // namespace tbcad::print
