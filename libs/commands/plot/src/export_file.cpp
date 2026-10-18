#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "tbcad/layout/layout.h"
#include "tbcad/platform/paths.h"
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
    // TBCAD_PLOT_TRACE=<text>: add to the result how each colour became a pen, and the colour and
    // plot flags of every layer whose name contains <text> (for finding why a fill prints wrong).
    const char* traceEnv = std::getenv("TBCAD_PLOT_TRACE");
    settings.traceColors = traceEnv && *traceEnv;
    auto sheet = buildPlotSheet(doc, host, settings);
    if (!sheet)
        return sheet.error();
    const std::string file = withExtension(path, format);
    // The default folder (Documents/TBCAD plots) may not exist yet.
    if (auto parent = platform::pathFromUtf8(file).parent_path(); !parent.empty())
        platform::ensureDirectory(parent);
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
    if (settings.traceColors) {
        std::set<std::string> lines(sheet.value().trace.begin(), sheet.value().trace.end());
        for (const std::string& l : lines)
            text += "\n" + l;
        for (const auto& [id, layer] : doc.layers()) {
            if (layer.name.find(traceEnv) == std::string::npos)
                continue;
            const auto eff = doc.effective(id);
            char b[200];
            std::snprintf(b, sizeof b,
                          "\nplot layer %s: colour kind %d index/r %d,%d,%d, plot %d, on %d, "
                          "frozen %d, transparency %d; effective colour kind %d index/r %d",
                          layer.name.c_str(), static_cast<int>(layer.color.kind), layer.color.r,
                          layer.color.g, layer.color.b, layer.plot ? 1 : 0, layer.on ? 1 : 0,
                          layer.frozen ? 1 : 0, static_cast<int>(layer.transparency),
                          static_cast<int>(eff.color.kind), eff.color.r);
            text += b;
        }
    }
    return text;
}

}  // namespace tbcad::plot
