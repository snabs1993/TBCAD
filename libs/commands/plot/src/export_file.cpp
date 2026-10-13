#include <cmath>
#include <cstdio>

#include "tbcad/layout/layout.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

Result<exportlib::PlotSheet> buildPlotSheet(const Document& doc, const cmd::IHost& host,
                                            exportlib::PlotSettings settings) {
    if (settings.area == exportlib::PlotArea::Display)
        settings.window = viewWindow(host);
    const std::string layoutName = plotLayout(host);
    auto sheet = layoutName.empty() ? exportlib::buildSheet(doc, settings)
                                    : layout::buildLayoutSheet(doc, layoutName, settings);
    if (sheet)
        addPlotStamp(sheet.value(), host, settings);
    return sheet;
}

Result<std::string> exportFile(const Document& doc, const cmd::IHost& host,
                               exportlib::PlotSettings settings, Format format,
                               const std::string& path) {
    const std::string layoutName = plotLayout(host);
    auto sheet = buildPlotSheet(doc, host, settings);
    if (!sheet)
        return sheet.error();
    const std::string file = withExtension(path, format);
    const double dpi = plotDpi(host);
    auto written = format == Format::Pdf   ? exportlib::writePdf(sheet.value(), file)
                   : format == Format::Svg ? exportlib::writeSvg(sheet.value(), file)
                   : format == Format::Png
                       ? exportlib::writePng(sheet.value(), file, dpi)
                       : exportlib::writeJpeg(sheet.value(), file, dpi, jpegQuality(host));
    if (!written)
        return written.error();
    const auto& lay = sheet.value().layout;
    char size[64], scale[32];
    std::snprintf(size, sizeof size, "%.0f x %.0f mm", lay.pageWidthMm, lay.pageHeightMm);
    std::snprintf(scale, sizeof scale, "1:%.4g", 1.0 / lay.paperPerUnit);
    std::string text = (layoutName.empty() ? std::string() : "Layout " + layoutName + ": ") +
                       "Plotted " + std::to_string(sheet.value().entityCount) + " objects to " +
                       file + " (" + settings.paperName + ", " + size + ", scale " + scale + ")";
    if (format == Format::Png || format == Format::Jpeg) {
        char res[48];
        std::snprintf(res, sizeof res, ", %.0f dpi", dpi);
        text.insert(text.size() - 1, res);
    }
    if (lay.overflows)
        text += ". Warning: the drawing is larger than the paper and was clipped.";
    return text;
}

}  // namespace tbcad::plot
