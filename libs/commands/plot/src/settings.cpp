#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "tbcad/platform/paths.h"
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

std::string plotStyleName(const cmd::IHost& host) {
    auto v = host.setting("plot.style");
    return v && !v->empty() ? *v : std::string(plotstyle::kDefaultTable);
}

void setPlotStyleName(cmd::IHost& host, const std::string& name) {
    host.setSetting("plot.style", name);
}

std::filesystem::path plotStyleFolder(const cmd::IHost& host) {
    auto base = host.settingsFolder();
    return base.empty() ? base : base / "plot styles";
}

namespace {

// AutoCAD keeps a user's plot style tables in
// %APPDATA%\Autodesk\AutoCAD <year>\R<n>\<language>\Plotters\Plot Styles.
void addAutoCadStyleFolders(std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    for (const char* var : {"APPDATA", "LOCALAPPDATA"}) {
        const auto base = platform::processEnvironment(var);
        if (!base || base->empty())
            continue;
        const std::filesystem::path autodesk = std::filesystem::path(*base) / "Autodesk";
        if (!std::filesystem::is_directory(autodesk, ec))
            continue;
        for (const auto& product : std::filesystem::directory_iterator(autodesk, ec))
            for (const auto& release : std::filesystem::directory_iterator(product.path(), ec))
                for (const auto& lang : std::filesystem::directory_iterator(release.path(), ec)) {
                    const auto folder = lang.path() / "Plotters" / "Plot Styles";
                    if (std::filesystem::is_directory(folder, ec))
                        out.push_back(folder);
                }
    }
}

}  // namespace

std::vector<std::filesystem::path> styleSearchFolders(const cmd::IHost& host) {
    std::vector<std::filesystem::path> out;
    const std::string doc = host.documentFile();
    if (!doc.empty()) {
        const auto dir = platform::pathFromUtf8(doc).parent_path();
        if (!dir.empty()) {
            out.push_back(dir);
            out.push_back(dir / "Plot Styles");
        }
    }
    if (auto own = plotStyleFolder(host); !own.empty())
        out.push_back(own);
    addAutoCadStyleFolders(out);
    return out;
}

Result<std::shared_ptr<const plotstyle::PlotStyleTable>> resolveStyleTable(
    const cmd::IHost& host, const std::string& name) {
    using Table = std::shared_ptr<const plotstyle::PlotStyleTable>;
    std::string lowered = name;
    for (char& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered.empty() || lowered == "none")
        return Table{};
    if (auto b = plotstyle::builtinTable(name))
        return Table(std::make_shared<plotstyle::PlotStyleTable>(std::move(*b)));
    std::filesystem::path path(name);
    std::error_code ec;
    if (!path.is_absolute() || !std::filesystem::exists(path, ec)) {
        bool found = false;
        for (const auto& folder : styleSearchFolders(host)) {
            for (const char* ext : {"", ".ctb", ".tbstyle"}) {
                auto candidate = folder / (name + ext);
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    path = candidate;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found)
            return Error{ErrorCode::NotFound, "Plot style table \"" + name +
                                                  "\" was not found next to the drawing, in the "
                                                  "TBCAD plot styles folder or in AutoCAD's."};
    }
    auto t = plotstyle::loadFile(path);
    if (!t)
        return Error{t.error().code, "Plot style table \"" + name + "\": " + t.error().message};
    return Table(std::make_shared<plotstyle::PlotStyleTable>(std::move(t.value())));
}

bool styleTableMissing(const cmd::IHost& host) {
    const std::string name = plotStyleName(host);
    if (name.empty())
        return false;
    return !resolveStyleTable(host, name).ok();
}

std::vector<std::string> listStyleTables(const cmd::IHost& host) {
    std::vector<std::string> out = plotstyle::builtinTableNames();
    auto folder = plotStyleFolder(host);
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
            auto ext = e.path().extension().string();
            if (ext == ".tbstyle" || ext == ".ctb")
                files.push_back(e.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        out.insert(out.end(), files.begin(), files.end());
    }
    return out;
}

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
        double vals[4] = {};
        const char* p = v->c_str();
        int n = 0;
        for (; n < 4; ++n) {
            char* end = nullptr;
            vals[n] = std::strtod(p, &end);
            if (end == p)
                break;
            p = end;
            if (*p == ',')
                ++p;
        }
        if (n == 4) {
            s.window.min = {vals[0], vals[1]};
            s.window.max = {vals[2], vals[3]};
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
    if (auto t = resolveStyleTable(h, plotStyleName(h)))
        s.styleTable = t.value();
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

const char* extension(Format f) {
    switch (f) {
        case Format::Pdf:
            return ".pdf";
        case Format::Svg:
            return ".svg";
        case Format::Png:
            return ".png";
        case Format::Jpeg:
            return ".jpg";
    }
    return ".pdf";
}

const char* formatName(Format f) {
    switch (f) {
        case Format::Pdf:
            return "PDF";
        case Format::Svg:
            return "SVG";
        case Format::Png:
            return "PNG";
        case Format::Jpeg:
            return "JPEG";
    }
    return "PDF";
}

double plotDpi(const cmd::IHost& h) {
    if (auto v = h.setting("plot.dpi"); v && !v->empty()) {
        const double d = std::atof(v->c_str());
        if (d >= 10 && d <= 2400)
            return d;
    }
    return 300;
}

void setPlotDpi(cmd::IHost& h, double dpi) {
    h.setSetting("plot.dpi", fmt(dpi));
}

int jpegQuality(const cmd::IHost& h) {
    if (auto v = h.setting("plot.jpegQuality"); v && !v->empty()) {
        const int q = std::atoi(v->c_str());
        if (q >= 1 && q <= 100)
            return q;
    }
    return 90;
}

namespace {
std::string fileKey(Format f) {
    return std::string("plot.file") + extension(f);  // plot.file.pdf, plot.file.svg, ...
}
}  // namespace

std::string lastFile(const cmd::IHost& h, Format f) {
    if (auto v = h.setting(fileKey(f)); v && !v->empty())
        return *v;
    return std::string("drawing") + extension(f);
}

std::string defaultFile(const cmd::IHost& h, Format f) {
    std::string doc = h.documentFile();
    if (doc.empty())
        return lastFile(h, f);
    auto dot = doc.find_last_of('.');
    auto slash = doc.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        doc.resize(dot);
    return doc + extension(f);
}

void setLastFile(cmd::IHost& h, Format f, const std::string& path) {
    h.setSetting(fileKey(f), path);
}

std::string plotLayout(const cmd::IHost& host) {
    return host.setting("plot.layout").value_or("");
}

void setPlotLayout(cmd::IHost& host, const std::string& name) {
    host.setSetting("plot.layout", name);
}

std::string withExtension(std::string path, Format f) {
    const std::string ext = extension(f);
    auto slash = path.find_last_of("/\\");
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        path += ext;
    return path;
}

std::string forFormat(std::string path, Format f) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        std::string ext = path.substr(dot);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".pdf" || ext == ".svg" || ext == ".png" || ext == ".jpg" || ext == ".jpeg")
            path.resize(dot);
    }
    return withExtension(std::move(path), f);
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

layout::StyleResolver styleResolver(const cmd::IHost& host) {
    return [&host](const std::string& name) -> std::shared_ptr<const plotstyle::PlotStyleTable> {
        auto t = resolveStyleTable(host, name);
        return t ? t.value() : nullptr;
    };
}

layout::PageSetup currentPageSetup(const cmd::IHost& host, const std::string& name) {
    return layout::makePageSetup(name, loadSettings(host), plotStyleName(host));
}

void applyPageSetup(cmd::IHost& host, const layout::PageSetup& setup) {
    const exportlib::PlotSettings now = loadSettings(host);
    exportlib::PlotSettings s = layout::settingsFor(now, setup, styleResolver(host));
    saveSettings(host, s);
    setPlotStyleName(host, setup.styleName.empty() ? "none" : setup.styleName);
}

std::string applyAssignedPageSetup(const Document& doc, cmd::IHost& host) {
    const std::string layoutName = plotLayout(host);
    const auto setup = layoutName.empty() ? layout::pageSetupOfModel(doc)
                                          : layout::pageSetupOfLayout(doc, layoutName);
    if (!setup)
        return {};
    applyPageSetup(host, *setup);
    std::string msg = "Page setup \"" + setup->name + "\" applied" +
                      (layoutName.empty() ? " (model space)." : " (layout " + layoutName + ").");
    if (!setup->styleName.empty() && styleTableMissing(host))
        msg +=
            " Plot style table \"" + setup->styleName +
            "\" was not found, so no style is applied; monochrome.ctb prints everything in black.";
    return msg;
}

}  // namespace tbcad::plot
