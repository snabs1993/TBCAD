#include <ctime>

#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {
constexpr const char* kDefaultStamp = "%file%  %layout%  %date% %time%  %scale%  %paper%";

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}
}  // namespace

bool plotStampOn(const cmd::IHost& host) {
    return host.setting("plot.stamp").value_or("0") == "1";
}

void setPlotStamp(cmd::IHost& host, bool on) {
    host.setSetting("plot.stamp", on ? "1" : "0");
}

std::string plotStampText(const cmd::IHost& host) {
    auto v = host.setting("plot.stampText");
    return v && !v->empty() ? *v : kDefaultStamp;
}

void setPlotStampText(cmd::IHost& host, const std::string& text) {
    host.setSetting("plot.stampText", text);
}

bool pdfLayersOn(const cmd::IHost& host) {
    return host.setting("plot.pdfLayers").value_or("1") != "0";
}

void setPdfLayers(cmd::IHost& host, bool on) {
    host.setSetting("plot.pdfLayers", on ? "1" : "0");
}

std::string expandStamp(const std::string& tmpl, const cmd::IHost& host,
                        const exportlib::PlotSheet& sheet,
                        const exportlib::PlotSettings& settings) {
    return expandStamp(tmpl, host.documentFile(), plotLayout(host), sheet, settings);
}

std::string expandStamp(const std::string& tmpl, const std::string& drawingFile,
                        const std::string& layoutName, const exportlib::PlotSheet& sheet,
                        const exportlib::PlotSettings& settings) {
    std::string out = tmpl;
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char date[16], clock[16], scale[32];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tmv);
    std::strftime(clock, sizeof clock, "%H:%M", &tmv);
    std::snprintf(scale, sizeof scale, "1:%.4g", 1.0 / sheet.layout.paperPerUnit);
    std::string file = baseName(drawingFile);
    if (file.empty())
        file = "Untitled";
    replaceAll(out, "%file%", file);
    replaceAll(out, "%layout%", layoutName.empty() ? "Model" : layoutName);
    replaceAll(out, "%date%", date);
    replaceAll(out, "%time%", clock);
    replaceAll(out, "%scale%", layoutName.empty() ? scale : "");
    replaceAll(out, "%paper%", settings.paperName);
    return out;
}

void addPlotStamp(exportlib::PlotSheet& sheet, const cmd::IHost& host,
                  const exportlib::PlotSettings& settings) {
    addPlotStamp(sheet, host, settings, host.documentFile(), plotLayout(host));
}

void addPlotStamp(exportlib::PlotSheet& sheet, const cmd::IHost& host,
                  const exportlib::PlotSettings& settings, const std::string& drawingFile,
                  const std::string& layoutName) {
    if (!plotStampOn(host))
        return;
    const std::string text =
        expandStamp(plotStampText(host), drawingFile, layoutName, sheet, settings);
    if (text.empty())
        return;
    // The stamp is a TEXT object in a scratch drawing, plotted 1:1 so it goes through the same
    // fonts and display lists as any other text.
    Document scratch;
    Entity e;
    TextData t;
    t.position = {sheet.layout.printable.min.x + 2, sheet.layout.printable.min.y + 1.5};
    t.height = 2.0;
    t.utf8 = text;
    e.data = t;
    auto tx = scratch.begin("stamp");
    auto id = tx.add(e);
    if (!id)
        return;
    tx.commit();
    exportlib::PlotLayout page = sheet.layout;
    page.paperPerUnit = 1;
    page.area = {};
    page.area.expand({0, 0});
    page.area.expand({sheet.layout.pageWidthMm, sheet.layout.pageHeightMm});
    page.origin = {0, 0};
    const std::size_t count = sheet.entityCount;
    exportlib::appendEntities(sheet, scratch, {id.value()}, page, settings);
    sheet.entityCount = count;  // the stamp is not one of the drawing's objects
}

}  // namespace tbcad::plot
