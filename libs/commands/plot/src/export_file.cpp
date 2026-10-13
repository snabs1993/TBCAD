#include <cmath>
#include <cstdio>

#include "tbcad/layout/layout.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

Result<std::string> exportFile(const Document& doc, const cmd::IHost& host,
                               exportlib::PlotSettings settings, Format format,
                               const std::string& path) {
    if (settings.area == exportlib::PlotArea::Display)
        settings.window = viewWindow(host);
    const std::string layoutName = plotLayout(host);
    auto sheet = layoutName.empty() ? exportlib::buildSheet(doc, settings)
                                    : layout::buildLayoutSheet(doc, layoutName, settings);
    if (!sheet)
        return sheet.error();
    const std::string file = withExtension(path, format);
    auto written = format == Format::Pdf ? exportlib::writePdf(sheet.value(), file)
                                         : exportlib::writeSvg(sheet.value(), file);
    if (!written)
        return written.error();
    const auto& lay = sheet.value().layout;
    char size[64], scale[32];
    std::snprintf(size, sizeof size, "%.0f x %.0f mm", lay.pageWidthMm, lay.pageHeightMm);
    std::snprintf(scale, sizeof scale, "1:%.4g", 1.0 / lay.paperPerUnit);
    std::string text = (layoutName.empty() ? std::string() : "Layout " + layoutName + ": ") +
                       "Plotted " + std::to_string(sheet.value().entityCount) + " objects to " +
                       file + " (" + settings.paperName + ", " + size + ", scale " + scale + ")";
    if (lay.overflows)
        text += ". Warning: the drawing is larger than the paper and was clipped.";
    return text;
}

}  // namespace tbcad::plot
