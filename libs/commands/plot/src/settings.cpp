#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "tbcad/plot/plot.h"

namespace tbcad::plot {

using exportlib::ColorMode;
using exportlib::PlotArea;
using exportlib::PlotSettings;

namespace {

std::string fmt(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

double getD(const cmd::IHost& h, const char* key, double fallback) {
    auto v = h.setting(key);
    if (!v || v->empty())
        return fallback;
    char* end = nullptr;
    double d = std::strtod(v->c_str(), &end);
    return end && *end == 0 ? d : fallback;
}

bool getB(const cmd::IHost& h, const char* key, bool fallback) {
    auto v = h.setting(key);
    if (!v || v->empty())
        return fallback;
    return *v == "1" || *v == "true" || *v == "on";
}

}  // namespace

PlotSettings loadSettings(const cmd::IHost& h) {
    PlotSettings s;
    if (auto v = h.setting("plot.paper"); v && !v->empty())
        s.paperName = *v;
    s.customWidthMm = getD(h, "plot.custom.width", s.customWidthMm);
    s.customHeightMm = getD(h, "plot.custom.height", s.customHeightMm);
    s.landscape = getB(h, "plot.landscape", s.landscape);
    if (auto v = h.setting("plot.area")) {
        if (*v == "display")
            s.area = PlotArea::Display;
        else if (*v == "window")
            s.area = PlotArea::Window;
    }
    if (auto v = h.setting("plot.window"); v && !v->empty()) {
        double a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(v->c_str(), "%lf,%lf,%lf,%lf", &a, &b, &c, &d) == 4) {
            s.window.min = {a, b};
            s.window.max = {c, d};
        }
    }
    s.fitToPaper = getB(h, "plot.fit", s.fitToPaper);
    s.scaleN = getD(h, "plot.scale", s.scaleN);
    s.centerOnPaper = getB(h, "plot.center", s.centerOnPaper);
    s.offsetMm = {getD(h, "plot.offset.x", 0), getD(h, "plot.offset.y", 0)};
    s.marginMm = getD(h, "plot.margin", s.marginMm);
    s.lineweights = getB(h, "plot.lineweights", s.lineweights);
    if (auto v = h.setting("plot.color")) {
        if (*v == "mono")
            s.color = ColorMode::Monochrome;
        else if (*v == "grey")
            s.color = ColorMode::Greyscale;
    }
    return s;
}

void saveSettings(cmd::IHost& h, const PlotSettings& s) {
    h.setSetting("plot.paper", s.paperName);
    h.setSetting("plot.custom.width", fmt(s.customWidthMm));
    h.setSetting("plot.custom.height", fmt(s.customHeightMm));
    h.setSetting("plot.landscape", s.landscape ? "1" : "0");
    h.setSetting("plot.area", s.area == PlotArea::Display  ? "display"
                              : s.area == PlotArea::Window ? "window"
                                                           : "extents");
    if (!s.window.empty())
        h.setSetting("plot.window", fmt(s.window.min.x) + "," + fmt(s.window.min.y) + "," +
                                        fmt(s.window.max.x) + "," + fmt(s.window.max.y));
    h.setSetting("plot.fit", s.fitToPaper ? "1" : "0");
    h.setSetting("plot.scale", fmt(s.scaleN));
    h.setSetting("plot.center", s.centerOnPaper ? "1" : "0");
    h.setSetting("plot.offset.x", fmt(s.offsetMm.x));
    h.setSetting("plot.offset.y", fmt(s.offsetMm.y));
    h.setSetting("plot.margin", fmt(s.marginMm));
    h.setSetting("plot.lineweights", s.lineweights ? "1" : "0");
    h.setSetting("plot.color", s.color == ColorMode::Monochrome  ? "mono"
                               : s.color == ColorMode::Greyscale ? "grey"
                                                                 : "color");
}

std::string lastFile(const cmd::IHost& h, Format f) {
    if (auto v = h.setting(f == Format::Pdf ? "plot.file.pdf" : "plot.file.svg"); v && !v->empty())
        return *v;
    return f == Format::Pdf ? "drawing.pdf" : "drawing.svg";
}

void setLastFile(cmd::IHost& h, Format f, const std::string& path) {
    h.setSetting(f == Format::Pdf ? "plot.file.pdf" : "plot.file.svg", path);
}

std::string withExtension(std::string path, Format f) {
    const std::string ext = f == Format::Pdf ? ".pdf" : ".svg";
    auto slash = path.find_last_of("/\\");
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        path += ext;
    return path;
}

BBox2 viewWindow(const cmd::IHost& h) {
    cmd::ViewState v = h.view();
    BBox2 b;
    if (!(v.scale > 0) || !(v.sizePx.x > 0) || !(v.sizePx.y > 0))
        return b;
    Vec2 half{v.sizePx.x * v.scale / 2, v.sizePx.y * v.scale / 2};
    b.min = v.center - half;
    b.max = v.center + half;
    return b;
}

bool parseScale(const std::string& text, PlotSettings& s) {
    std::string t;
    for (char c : text)
        if (c != ' ')
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "fit" || t == "f") {
        s.fitToPaper = true;
        return true;
    }
    auto colon = t.find(':');
    std::string num = colon == std::string::npos ? t : t.substr(colon + 1);
    if (colon != std::string::npos && t.substr(0, colon) != "1")
        return false;
    char* end = nullptr;
    double n = std::strtod(num.c_str(), &end);
    if (num.empty() || !end || *end != 0 || !(n > 0))
        return false;
    s.fitToPaper = false;
    s.scaleN = n;
    return true;
}

std::string scaleText(const PlotSettings& s) {
    return s.fitToPaper ? "Fit" : "1:" + fmt(s.scaleN);
}

}  // namespace tbcad::plot
