#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"
#include "tbcad/sheets/sheets.h"

namespace tbcad::plot {

std::unique_ptr<cmd::Command> makePrinterCommand();
std::unique_ptr<cmd::Command> makeETransmitCommand();
std::unique_ptr<cmd::Command> makeCompareCommand();
std::unique_ptr<cmd::Command> makePlotFrameCommand();

Result<std::unique_ptr<Document>> readDrawingFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".dxf") {
        io::IoReport report;
        return io::readTbcad(file, report);
    }
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Error{ErrorCode::Io, "cannot read " + platform::pathToUtf8(file)};
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    dxf::Report report;
    auto r =
        dxf::importDxf({reinterpret_cast<const std::byte*>(raw.data()), raw.size()}, {}, report);
    if (!r)
        return r.error();
    return std::move(r.value().document);
}

namespace {

using cmd::KeywordOptions;
using cmd::Status;
using exportlib::ColorMode;
using exportlib::PlotArea;
using exportlib::PlotSettings;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string styleLabel(const PlotSettings& s) {
    return s.styleTable ? (s.styleTable->name.empty() ? "styled" : s.styleTable->name) : "no style";
}

// The area a named view (VIEW command, header "tbcad.view.<name>" = "cx cy scale [w h]\t...")
// showed, as a plot window; empty for an unknown view or one saved without its size.
std::optional<BBox2> namedViewWindow(const Document& doc, const std::string& name) {
    constexpr std::string_view kPrefix = "tbcad.view.";
    auto lower = [](std::string t) {
        for (char& c : t)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    for (const auto& [key, value] : doc.header().custom) {
        if (key.rfind(kPrefix, 0) != 0 || lower(key.substr(kPrefix.size())) != lower(name))
            continue;
        std::istringstream in(value.substr(0, value.find('\t')));
        double cx = 0, cy = 0, scale = 0, w = 0, h = 0;
        if (!(in >> cx >> cy >> scale >> w >> h) || !(w > 0) || !(h > 0))
            return std::nullopt;
        BBox2 box;
        box.expand({cx - w / 2, cy - h / 2});
        box.expand({cx + w / 2, cy + h / 2});
        return box;
    }
    return std::nullopt;
}

std::string summary(const Document& doc, const cmd::IHost& host, const PlotSettings& s,
                    Format format) {
    std::string t = formatName(format);
    if (format == Format::Png || format == Format::Jpeg)
        t += " " + std::to_string(static_cast<int>(plotDpi(host) + 0.5)) + " dpi";
    // Plotting a layout uses the layout's own sheet, whatever the page setup says.
    const std::string onSheet = plotLayout(host);
    const auto sheet = onSheet.empty() ? std::nullopt : layout::findLayout(doc, onSheet);
    if (sheet) {
        std::ostringstream os;
        os << ", sheet \"" << sheet->name << "\" " << std::lround(sheet->widthMm) << " x "
           << std::lround(sheet->heightMm) << " mm";
        t += os.str();
    } else {
        t += ", " + s.paperName + (s.landscape ? " landscape" : " portrait");
    }
    if (sheet) {
        // The sheet plots at 1:1; the area and scale settings are for model space.
        t += ", scale 1:1 (viewports keep their own scales)";
        t += ", " + styleLabel(s);
        return t;
    }
    t += s.area == PlotArea::Extents   ? ", Extents"
         : s.area == PlotArea::Display ? ", Display"
                                       : ", Window";
    t += ", scale " + scaleText(s);
    t += ", " + styleLabel(s);
    return t;
}

// A layout plots on its own sheet, so a paper size or orientation picked while plotting one
// changes the sheet (viewports and objects keep their places), and the summary shows it.
void sheetFollows(cmd::CommandContext& ctx, const PlotSettings& s) {
    const std::string onSheet = plotLayout(ctx.host());
    const auto sheet = onSheet.empty() ? std::nullopt : layout::findLayout(ctx.doc(), onSheet);
    if (!sheet)
        return;
    auto r = layout::setLayoutPaper(ctx.tx(), sheet->name, s.paperName, s.landscape,
                                    s.customWidthMm, s.customHeightMm);
    if (!r)
        ctx.message(r.error().message);
}

// PLOT, EXPORTPDF and EXPORTSVG: one option menu, then the output file.
class PlotCommand final : public cmd::Command {
public:
    PlotCommand(std::string name, std::vector<std::string> aliases, std::string description,
                Format format)
        : name_(std::move(name)),
          aliases_(std::move(aliases)),
          description_(std::move(description)),
          format_(format) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        PlotSettings s;
        timePhase("load settings", [&] {
            if (const std::string m = followPlottedLayoutSetup(ctx.doc(), ctx.host()); !m.empty())
                ctx.message(m);
            s = loadSettings(ctx.host());
        });
        Format format = format_;
        // Strict (set by scripted callers such as the MCP tools): a value that cannot be used ends
        // the command with an "Error:" message instead of falling back to the old setting.
        bool strict = false;
        bool colorChosen = false;  // Color picked in this run (warn only then)
        // Area Frame picked: annotative objects print at the frame's scale.
        bool frameArea = false;
        for (;;) {
            KeywordOptions menu;
            menu.keywords = {"Paper",   "Orientation", "Area",        "Scale",        "Center",
                             "Offset",  "Margin",      "LineWeights", "Transparency", "Color",
                             "Style",   "Format",      "Resolution",  "Layout",       "Stamp",
                             "LWScale", "Screening",   "PdfLayers",   "UpsideDown",   "Strict",
                             "Export"};
            menu.defaultKeyword = "Export";
            std::string prompt;
            timePhase("menu summary",
                      [&] { prompt = "Plot " + summary(ctx.doc(), ctx.host(), s, format); });
            auto choice = co_await ctx.getKeyword(prompt, menu);
            if (choice.status == Status::Cancel)
                co_return;
            const std::string& kw = choice.keyword;

            if (kw == "Strict") {
                strict = true;
                continue;
            }
            if (kw == "Export") {
                // Never a question here: scripts and headless plots must run through. The
                // drawing keeps its object colours, as AutoCAD does without the table.
                bool missing = false;
                timePhase("style check",
                          [&] { missing = !s.styleTable && styleTableMissing(ctx.host()); });
                if (missing)
                    ctx.message("Plot style table \"" + plotStyleName(ctx.host()) +
                                "\" was not found: plotting with object colours (Style picks "
                                "another table, e.g. monochrome.ctb).");
                if (colorChosen && s.color == ColorMode::AsDisplayed && s.styleTable &&
                    lower(plotStyleName(ctx.host())).find("monochrome") != std::string::npos)
                    ctx.message("Colour mode is Color, but the plot style table \"" +
                                plotStyleName(ctx.host()) +
                                "\" prints everything in black. Pick Style None or another table "
                                "for colour.");
                cmd::StringOptions fo;
                fo.defaultValue = defaultFile(ctx.host(), format);
                auto file = co_await ctx.getString("Output file", fo);
                if (!file.ok())
                    co_return;
                std::error_code existsEc;
                // With FILEDIA 0 (scripts, command-line use) an existing file is replaced without
                // a question: nobody is there to answer it.
                const bool askOverwrite = ctx.host().setting("files.filedia").value_or("1") != "0";
                if (askOverwrite &&
                    std::filesystem::exists(
                        platform::pathFromUtf8(withExtension(file.text, format)), existsEc)) {
                    KeywordOptions ow;
                    ow.keywords = {"Yes", "No"};
                    ow.defaultKeyword = "No";
                    auto over = co_await ctx.getKeyword(
                        "\"" + withExtension(file.text, format) + "\" already exists. Overwrite",
                        ow);
                    if (over.status == Status::Cancel)
                        co_return;
                    if (!over.ok() || over.keyword != "Yes")
                        continue;  // back to the plot options to pick another name
                }
                s.annoScale = frameArea && !s.fitToPaper ? s.scaleN : 0;
                Result<std::string> r = std::string();
                timePhase("export file",
                          [&] { r = exportFile(ctx.doc(), ctx.host(), s, format, file.text); });
                if (!r) {
                    ctx.message(r.error().message);
                    continue;  // let the user fix the settings and try again
                }
                timePhase("save settings", [&] {
                    saveSettings(ctx.host(), s);
                    setLastFile(ctx.host(), format, withExtension(file.text, format));
                });
                ctx.message(r.value());
                co_return;
            }
            if (kw == "Paper") {
                cmd::StringOptions po;
                po.defaultValue = s.paperName;
                auto name = co_await ctx.getString("Paper size (A0-A4, ANSI A-E, Custom)", po);
                if (!name.ok())
                    continue;
                if (lower(name.text) == "custom") {
                    cmd::RealOptions ro;
                    ro.min = 1;
                    ro.defaultValue = s.customWidthMm;
                    auto w = co_await ctx.getReal("Paper width in mm", ro);
                    if (!w.ok())
                        continue;
                    ro.defaultValue = s.customHeightMm;
                    auto h = co_await ctx.getReal("Paper height in mm", ro);
                    if (!h.ok())
                        continue;
                    s.paperName = "Custom";
                    s.customWidthMm = std::min(w.value, h.value);
                    s.customHeightMm = std::max(w.value, h.value);
                } else {
                    const exportlib::PaperSize* found = nullptr;
                    for (const auto& p : exportlib::paperSizes())
                        if (lower(p.name) == lower(name.text))
                            found = &p;
                    if (!found) {
                        ctx.message(std::string(strict ? "Error: " : "") + "Unknown paper size \"" +
                                    name.text + "\".");
                        if (strict)
                            co_return;
                        continue;
                    }
                    s.paperName = found->name;
                }
                sheetFollows(ctx, s);
            } else if (kw == "UpsideDown") {
                KeywordOptions ko;
                ko.keywords = {"Yes", "No"};
                ko.defaultKeyword = s.upsideDown ? "Yes" : "No";
                auto r = co_await ctx.getKeyword("Plot upside down", ko);
                if (r.ok())
                    s.upsideDown = r.keyword == "Yes";
            } else if (kw == "Orientation") {
                KeywordOptions ko;
                ko.keywords = {"Landscape", "Portrait"};
                ko.defaultKeyword = s.landscape ? "Landscape" : "Portrait";
                auto r = co_await ctx.getKeyword("Orientation", ko);
                if (r.ok()) {
                    s.landscape = r.keyword == "Landscape";
                    sheetFollows(ctx, s);
                }
            } else if (kw == "Area") {
                KeywordOptions ko;
                ko.keywords = {"Extents", "Display", "Window", "View"};
                const auto frames = layout::plotFrames(ctx.doc());
                if (!frames.empty() && plotLayout(ctx.host()).empty())
                    ko.keywords.push_back("Frame");
                ko.defaultKeyword = s.area == PlotArea::Extents   ? "Extents"
                                    : s.area == PlotArea::Display ? "Display"
                                                                  : "Window";
                auto r = co_await ctx.getKeyword("Plot area", ko);
                if (!r.ok())
                    continue;
                frameArea = false;
                if (r.keyword == "Frame") {
                    cmd::StringOptions fo;
                    fo.defaultValue = frames.front().name;
                    auto n = co_await ctx.getString("Plot frame", fo);
                    if (!n.ok())
                        continue;
                    if (auto f = layout::findPlotFrame(ctx.doc(), n.text)) {
                        s = layout::frameSettings(ctx.doc(), s, *f, styleResolver(ctx.host()));
                        frameArea = true;
                    } else {
                        ctx.message("There is no plot frame named \"" + n.text + "\".");
                    }
                } else if (r.keyword == "Window") {
                    auto a = co_await ctx.getPoint("Specify first corner");
                    if (!a.ok())
                        continue;
                    cmd::PointOptions po;
                    po.base = a.point;
                    auto b = co_await ctx.getPoint("Specify opposite corner", po);
                    if (!b.ok())
                        continue;
                    s.window = {};
                    s.window.expand(a.point);
                    s.window.expand(b.point);
                    s.area = PlotArea::Window;
                } else if (r.keyword == "View") {
                    auto n = co_await ctx.getString("Named view");
                    if (!n.ok())
                        continue;
                    if (auto w = namedViewWindow(ctx.doc(), n.text)) {
                        s.window = *w;
                        s.area = PlotArea::Window;
                    } else {
                        ctx.message("There is no view named \"" + n.text +
                                    "\" (or it has no saved area).");
                    }
                } else {
                    s.area = r.keyword == "Extents" ? PlotArea::Extents : PlotArea::Display;
                }
            } else if (kw == "Scale") {
                cmd::StringOptions so;
                so.defaultValue = scaleText(s);
                auto r = co_await ctx.getString("Scale (Fit or 1:N, for example 1:100)", so);
                if (r.ok() && !parseScale(r.text, s)) {
                    ctx.message(std::string(strict ? "Error: " : "") + "Not a scale: \"" + r.text +
                                "\". Use Fit or 1:N.");
                    if (strict)
                        co_return;
                }
            } else if (kw == "Center") {
                KeywordOptions ko;
                ko.keywords = {"Yes", "No"};
                ko.defaultKeyword = s.centerOnPaper ? "Yes" : "No";
                auto r = co_await ctx.getKeyword("Centre on paper", ko);
                if (r.ok())
                    s.centerOnPaper = r.keyword == "Yes";
            } else if (kw == "Offset") {
                cmd::RealOptions ro;
                ro.defaultValue = s.offsetMm.x;
                auto x = co_await ctx.getReal("Offset X from the margin in mm", ro);
                if (!x.ok())
                    continue;
                ro.defaultValue = s.offsetMm.y;
                auto y = co_await ctx.getReal("Offset Y from the margin in mm", ro);
                if (!y.ok())
                    continue;
                s.offsetMm = {x.value, y.value};
                s.centerOnPaper = false;
            } else if (kw == "Margin") {
                cmd::RealOptions ro;
                ro.min = 0;
                ro.defaultValue = s.marginMm;
                auto r = co_await ctx.getReal("Margin in mm", ro);
                if (r.ok())
                    s.marginMm = r.value;
            } else if (kw == "LineWeights") {
                KeywordOptions ko;
                ko.keywords = {"On", "Off"};
                ko.defaultKeyword = s.lineweights ? "On" : "Off";
                auto r = co_await ctx.getKeyword("Plot lineweights", ko);
                if (r.ok())
                    s.lineweights = r.keyword == "On";
            } else if (kw == "LWScale") {
                cmd::RealOptions ro;
                ro.defaultValue = s.lineweightScale;
                ro.min = 0.01;
                ro.max = 100;
                auto r = co_await ctx.getReal("Lineweight scale (1 = as drawn)", ro);
                if (r.ok())
                    s.lineweightScale = r.value;
            } else if (kw == "Screening") {
                cmd::IntOptions io;
                io.defaultValue = s.screening;
                io.min = 0;
                io.max = 100;
                auto r = co_await ctx.getInteger("Screening % (100 = full tone)", io);
                if (r.ok())
                    s.screening = static_cast<int>(r.value);
            } else if (kw == "Transparency") {
                KeywordOptions ko;
                ko.keywords = {"On", "Off"};
                ko.defaultKeyword = s.transparency ? "On" : "Off";
                auto r = co_await ctx.getKeyword("Plot transparency", ko);
                if (r.ok())
                    s.transparency = r.keyword == "On";
            } else if (kw == "Color") {
                KeywordOptions ko;
                ko.keywords = {"Color", "Monochrome", "Greyscale"};
                ko.defaultKeyword = s.color == ColorMode::Monochrome  ? "Monochrome"
                                    : s.color == ColorMode::Greyscale ? "Greyscale"
                                                                      : "Color";
                auto r = co_await ctx.getKeyword("Colour mode", ko);
                colorChosen = r.ok();
                if (r.ok())
                    s.color = r.keyword == "Monochrome"  ? ColorMode::Monochrome
                              : r.keyword == "Greyscale" ? ColorMode::Greyscale
                                                         : ColorMode::AsDisplayed;
            } else if (kw == "Style") {
                cmd::StringOptions so;
                so.defaultValue = plotStyleName(ctx.host());
                auto r = co_await ctx.getString("Plot style table (name, file, or None)", so);
                if (!r.ok())
                    continue;
                auto t = resolveStyleTable(ctx.host(), r.text);
                if (!t) {
                    ctx.message(std::string(strict ? "Error: " : "") + t.error().message);
                    if (strict)
                        co_return;
                    continue;
                }
                setPlotStyleName(ctx.host(), r.text);
                s.styleTable = t.value();
            } else if (kw == "Layout") {
                cmd::StringOptions so;
                so.defaultValue = plotLayout(ctx.host());
                auto r = co_await ctx.getString("Layout to plot (empty = model space)", so);
                if (r.ok()) {
                    // "Model", as the tab is called, is model space too.
                    std::string name = r.text;
                    if (name.size() == 5 &&
                        std::tolower(static_cast<unsigned char>(name[0])) == 'm' &&
                        std::equal(name.begin() + 1, name.end(), "odel", [](char a, char b) {
                            return std::tolower(static_cast<unsigned char>(a)) == b;
                        }))
                        name.clear();
                    std::string m;
                    timePhase("layout page setup", [&] {
                        setPlotLayout(ctx.host(), name);
                        ctx.host().setSetting("tmp.plot.setupFollowed", "");
                        m = followPlottedLayoutSetup(ctx.doc(), ctx.host());
                    });
                    if (!m.empty()) {
                        s = loadSettings(ctx.host());
                        ctx.message(m);
                    }
                }
            } else if (kw == "Format") {
                KeywordOptions ko;
                ko.keywords = {"Pdf", "Svg", "Png", "Jpeg"};
                ko.defaultKeyword = format == Format::Pdf   ? "Pdf"
                                    : format == Format::Svg ? "Svg"
                                    : format == Format::Png ? "Png"
                                                            : "Jpeg";
                auto r = co_await ctx.getKeyword("Output format", ko);
                if (r.ok())
                    format = r.keyword == "Pdf"   ? Format::Pdf
                             : r.keyword == "Svg" ? Format::Svg
                             : r.keyword == "Png" ? Format::Png
                                                  : Format::Jpeg;
            } else if (kw == "Stamp") {
                KeywordOptions ko;
                ko.keywords = {"On", "Off"};
                ko.defaultKeyword = plotStampOn(ctx.host()) ? "On" : "Off";
                auto r = co_await ctx.getKeyword("Plot stamp (file, layout, date, scale)", ko);
                if (!r.ok())
                    continue;
                setPlotStamp(ctx.host(), r.keyword == "On");
                if (r.keyword == "On") {
                    cmd::StringOptions so;
                    so.defaultValue = plotStampText(ctx.host());
                    auto t = co_await ctx.getString(
                        "Stamp text (%file% %layout% %date% %time% %scale% %paper%)", so);
                    if (t.ok() && !t.text.empty())
                        setPlotStampText(ctx.host(), t.text);
                }
            } else if (kw == "PdfLayers") {
                KeywordOptions ko;
                ko.keywords = {"Yes", "No"};
                ko.defaultKeyword = pdfLayersOn(ctx.host()) ? "Yes" : "No";
                auto r = co_await ctx.getKeyword("Include layer information in the PDF", ko);
                if (r.ok())
                    setPdfLayers(ctx.host(), r.keyword == "Yes");
            } else if (kw == "Resolution") {
                cmd::RealOptions ro;
                ro.min = 10;
                ro.max = 2400;
                ro.defaultValue = plotDpi(ctx.host());
                auto r = co_await ctx.getReal("Resolution of PNG / JPEG output in dpi", ro);
                if (r.ok())
                    setPlotDpi(ctx.host(), r.value);
            }
            saveSettings(ctx.host(), s);  // preferences stick even if the plot is cancelled
        }
    }

private:
    std::string name_;
    std::vector<std::string> aliases_;
    std::string description_;
    Format format_;
};

// PLOTPANEL: shows the Plot panel (settings with live preview).
class PlotPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTPANEL"; }
    std::string_view description() const override { return "Show the Plot panel with preview"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("plot");
        co_return;
    }
};

// PAGESETUPPANEL: the Page Setup Manager (PAGESETUP is its typed form).
class PageSetupPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAGESETUPPANEL"; }
    std::string_view description() const override {
        return "Show the Page Setup Manager: the drawing's page setups and which sheet uses which";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("pagesetup");
        co_return;
    }
};

// PREVIEW: the current sheet (or the model) as it will plot, in a panel of its own.
class PreviewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PREVIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"PRE"}; }
    std::string_view description() const override {
        return "Show how the drawing or the current layout will plot";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("plotpreview");
        co_return;
    }
};

// The open drawing stands in for its file (unsaved changes plot), its page setups serve as
// overrides for sheets of other drawings, .dxf drawings are read too, and each sheet gets the plot
// stamp (with its own file and layout) and loses its layer list when PDF layers are off.
sheets::BuildOptions publishOptions(const Document& doc, const cmd::IHost& host) {
    sheets::BuildOptions o;
    o.current = &doc;
    std::string file = host.documentFile();
    if (file.empty())
        file = host.setting("document.file").value_or("");
    o.currentFile = platform::pathFromUtf8(file);
    o.setupSource = &doc;
    o.load = readDrawingFile;
    const bool layers = pdfLayersOn(host);
    o.decorate = [&host, layers](exportlib::PlotSheet& sheet, const sheets::SheetEntry& entry,
                                 const PlotSettings& settings) {
        addPlotStamp(sheet, host, settings, platform::pathToUtf8(entry.file), entry.layout);
        if (!layers)
            exportlib::dropLayers(sheet);
    };
    return o;
}

// PLOTSET: plots every sheet of a sheet list (see tbcad/sheets/sheets.h) into one PDF.
class PlotSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTSET"; }
    std::vector<std::string> defaultAliases() const override { return {"BATCHPLOT", "-PUBLISH"}; }
    std::string_view description() const override {
        return "Plot a list of layouts from several drawings into one PDF or onto a printer";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions lo;
        auto list = co_await ctx.getString("Sheet list file", lo);
        if (!list.ok())
            co_return;
        cmd::StringOptions fo;
        {
            auto dot = list.text.rfind('.');
            auto slash = list.text.find_last_of("/\\");
            bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
            fo.defaultValue = (hasExt ? list.text.substr(0, dot) : list.text) + ".pdf";
        }
        auto file =
            co_await ctx.getString("Output PDF file (or Printer / Dialog to print the sheets)", fo);
        if (!file.ok())
            co_return;
        auto entries = sheets::readSheetList(list.text);
        if (!entries) {
            ctx.message(entries.error().message);
            co_return;
        }
        if (lower(file.text) == "printer" || lower(file.text) == "dialog") {
            if (!print::available()) {
                ctx.message(
                    "Printing is only available in the Windows version. Plot to a PDF instead.");
                co_return;
            }
            auto set = sheets::buildSheetList(entries.value(), loadSettings(ctx.host()),
                                              styleResolver(ctx.host()),
                                              publishOptions(ctx.doc(), ctx.host()));
            if (!set) {
                ctx.message(set.error().message);
                co_return;
            }
            for (const auto& w : set.value().warnings)
                ctx.message("Warning: " + w);
            auto r = print::printSheets(set.value().sheets,
                                        printJob(ctx.host(), lower(file.text) == "dialog"));
            ctx.message(r ? "Sent " + std::to_string(set.value().sheets.size()) +
                                " sheets to the printer."
                          : r.error().message);
            co_return;
        }
        auto out = withExtension(file.text, Format::Pdf);
        auto r =
            sheets::plotSheetList(entries.value(), loadSettings(ctx.host()), out,
                                  styleResolver(ctx.host()), publishOptions(ctx.doc(), ctx.host()));
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        for (const auto& w : r.value().warnings)
            ctx.message("Warning: " + w);
        ctx.message("Plotted " + std::to_string(r.value().pages) + " sheets to " + out);
    }
};

// PUBLISH: the sheet list dialog (drawings, layouts, page setup per sheet, one multi-page PDF).
// -PUBLISH / BATCHPLOT is the command-line form (PLOTSET).
class PublishCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PUBLISH"; }
    std::vector<std::string> defaultAliases() const override {
        return {"SHEETLIST", "SHEETSET", "SSM"};
    }
    std::string_view description() const override {
        return "Make a list of sheets from several drawings and publish them to one PDF";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("publish");
        co_return;
    }
};

// PLOTSTAMP: switches the plot stamp and sets its text.
class PlotStampCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTSTAMP"; }
    std::string_view description() const override {
        return "Print the file name, layout, date and scale on every plot";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            KeywordOptions ko;
            ko.keywords = {"On", "Off", "Text"};
            ko.defaultKeyword = plotStampOn(ctx.host()) ? "Off" : "On";
            const std::string state = plotStampOn(ctx.host()) ? "on" : "off";
            const std::string prompt =
                "Plot stamp is " + state + ": \"" + plotStampText(ctx.host()) + "\"";
            auto r = co_await ctx.getKeyword(prompt, ko);
            if (!r.ok())
                co_return;
            if (r.keyword == "Text") {
                cmd::StringOptions so;
                so.defaultValue = plotStampText(ctx.host());
                auto t = co_await ctx.getString(
                    "Stamp text (%file% %layout% %date% %time% %scale% %paper%)", so);
                if (t.ok() && !t.text.empty())
                    setPlotStampText(ctx.host(), t.text);
                continue;
            }
            setPlotStamp(ctx.host(), r.keyword == "On");
            ctx.message(std::string("Plot stamp ") + (r.keyword == "On" ? "on." : "off."));
            co_return;
        }
    }
};

// PLOTSTYLE: choose, list, import and edit plot style tables.
class PlotStyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTSTYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"CTB"}; }
    std::string_view description() const override {
        return "Choose, list, import and edit plot style tables";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        KeywordOptions menu;
        menu.keywords = {"Select", "List", "Import", "Edit", "Save", "Paths", "Pen", "Raw"};
        menu.defaultKeyword = "List";
        auto choice =
            co_await ctx.getKeyword("Plot style table: " + plotStyleName(ctx.host()), menu);
        if (!choice.ok())
            co_return;
        const std::string& kw = choice.keyword;
        if (kw == "Raw") {  // the stored text of one pen in a .ctb file (to check the importer)
            cmd::StringOptions so;
            auto file = co_await ctx.getString("Path of the .ctb file", so);
            if (!file.ok())
                co_return;
            cmd::IntOptions io;
            io.defaultValue = 8;
            io.min = 1;
            io.max = 255;
            auto n = co_await ctx.getInteger("Colour index (1-255)", io);
            if (!n.ok())
                co_return;
            std::ifstream in(platform::pathFromUtf8(file.text), std::ios::binary);
            std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
            auto text =
                plotstyle::ctbText({reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()});
            if (!text) {
                ctx.message(text.error().message);
                co_return;
            }
            // Style k (0-based) is colour k+1; print its block and the lines above the styles.
            const std::string start = "\n " + std::to_string(n.value - 1) + "{";
            const auto at = text.value().find(start);
            if (at == std::string::npos) {
                ctx.message("No such pen in the file.");
                co_return;
            }
            const auto end = text.value().find("\n }", at);
            ctx.message(text.value().substr(at + 1, (end == std::string::npos ? 400 : end - at)));
            co_return;
        }
        if (kw == "Pen") {  // what the table does to one colour (palette index 1..255)
            auto t = resolveStyleTable(ctx.host(), plotStyleName(ctx.host()));
            if (!t || !t.value()) {
                ctx.message(t ? "No plot style table is selected." : t.error().message);
                co_return;
            }
            if (t.value()->named) {
                for (const auto& st : t.value()->styles)
                    ctx.message("  " + st.name + ": screening " + std::to_string(st.screen) + "%" +
                                (st.color ? ", own colour" : ", object colour") +
                                (st.lineweightMm
                                     ? ", lineweight " + std::to_string(*st.lineweightMm) + " mm"
                                     : std::string(", object lineweight")));
                co_return;
            }
            cmd::IntOptions io;
            io.defaultValue = 7;
            io.min = 1;
            io.max = 255;
            auto n = co_await ctx.getInteger("Colour index (1-255)", io);
            if (!n.ok())
                co_return;
            const auto& e = t.value()->forIndex(static_cast<int>(n.value));
            std::string line =
                "Pen " + std::to_string(n.value) + " in " + plotStyleName(ctx.host()) + ": ";
            line += e.color ? "colour " + std::to_string(e.color->r) + "," +
                                  std::to_string(e.color->g) + "," + std::to_string(e.color->b)
                            : std::string("object colour");
            line += ", screening " + std::to_string(e.screen) + "%";
            line += e.grayscale ? ", greyscale" : "";
            line += e.lineweightMm ? ", lineweight " + std::to_string(*e.lineweightMm) + " mm"
                                   : std::string(", object lineweight");
            ctx.message(line);
            co_return;
        }
        if (kw == "Paths") {
            cmd::KeywordOptions po;
            po.keywords = {"Add", "Remove", "Reset", "Show"};
            po.defaultKeyword = "Show";
            auto act = co_await ctx.getKeyword("Plot style folders", po);
            if (!act.ok())
                co_return;
            auto joined = [](const std::vector<std::filesystem::path>& v) {
                std::string out;
                for (const auto& f : v)
                    out += (out.empty() ? "" : ";") + platform::pathToUtf8(f);
                return out;
            };
            auto mine = userStyleFolders(ctx.host());
            if (act.keyword == "Add" || act.keyword == "Remove") {
                cmd::StringOptions so;
                auto dir = co_await ctx.getString(
                    act.keyword == "Add" ? "Folder to search" : "Folder to stop searching", so);
                if (!dir.ok() || dir.text.empty())
                    co_return;
                const auto path = platform::pathFromUtf8(dir.text);
                mine.erase(std::remove(mine.begin(), mine.end(), path), mine.end());
                if (act.keyword == "Add")
                    mine.push_back(path);
                ctx.host().setSetting("plot.stylepaths", joined(mine));
            } else if (act.keyword == "Reset") {
                ctx.host().setSetting("plot.stylepaths", "");
            }
            for (const auto& f : styleSearchFolders(ctx.host()))
                ctx.message("  " + platform::pathToUtf8(f));
            co_return;
        }
        if (kw == "List") {
            const std::string cur = plotStyleName(ctx.host());
            for (const auto& n : listStyleTables(ctx.host()))
                ctx.message((n == cur ? "* " : "  ") + n);
            co_return;
        }
        if (kw == "Select") {
            cmd::StringOptions so;
            so.defaultValue = plotStyleName(ctx.host());
            auto r = co_await ctx.getString("Table name, file path, or None", so);
            if (!r.ok())
                co_return;
            auto t = resolveStyleTable(ctx.host(), r.text);
            if (!t) {
                ctx.message(t.error().message);
                co_return;
            }
            setPlotStyleName(ctx.host(), r.text);
            ctx.message("Plot style table: " + (r.text.empty() ? std::string("None") : r.text));
            co_return;
        }
        if (kw == "Import") {
            cmd::StringOptions so;
            auto file = co_await ctx.getString("Path of the .ctb or .stb file to import", so);
            if (!file.ok())
                co_return;
            auto folder = plotStyleFolder(ctx.host());
            if (folder.empty()) {
                ctx.message("There is no folder for plot style tables on this system.");
                co_return;
            }
            auto t = plotstyle::loadFile(file.text);
            if (!t) {
                ctx.message(t.error().message);
                co_return;
            }
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            auto name = std::filesystem::path(file.text).stem().string();
            auto saved = plotstyle::saveFile(t.value(), folder / (name + ".tbstyle"));
            if (!saved) {
                ctx.message(saved.error().message);
                co_return;
            }
            setPlotStyleName(ctx.host(), name + ".tbstyle");
            ctx.message("Imported " + name + " and selected it.");
            co_return;
        }
        if (kw == "Save") {  // copies the current table under a new name so it can be edited
            auto folder = plotStyleFolder(ctx.host());
            auto cur = resolveStyleTable(ctx.host(), plotStyleName(ctx.host()));
            if (folder.empty() || !cur || !cur.value()) {
                ctx.message("Nothing to save: choose a plot style table first.");
                co_return;
            }
            cmd::StringOptions so;
            auto n = co_await ctx.getString("Name for the copy", so);
            if (!n.ok() || n.text.empty())
                co_return;
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            plotstyle::PlotStyleTable copy = *cur.value();
            copy.name = n.text;
            auto saved = plotstyle::saveFile(copy, folder / (n.text + ".tbstyle"));
            if (!saved) {
                ctx.message(saved.error().message);
                co_return;
            }
            setPlotStyleName(ctx.host(), n.text + ".tbstyle");
            ctx.message("Saved " + n.text + ".tbstyle and selected it.");
            co_return;
        }
        ctx.host().openPanel("plotstyle");  // Edit
    }
};

// The plot options of a named page setup (PLOTSETTINGS object) of a DXF file.
layout::PageSetup pageSetupOf(const dxf::PlotSetupRecord& r) {
    layout::PageSetup p;
    p.name = r.name;
    exportlib::PlotSettings& s = p.settings;
    const double lo = std::min(r.paperWidthMm, r.paperHeightMm);
    const double hi = std::max(r.paperWidthMm, r.paperHeightMm);
    s.paperName = "Custom";
    for (const exportlib::PaperSize& ps : exportlib::paperSizes())
        if (std::abs(ps.shortMm - lo) < 1.5 && std::abs(ps.longMm - hi) < 1.5) {
            s.paperName = ps.name;
            break;
        }
    if (s.paperName == "Custom" && lo > 0 && hi > 0) {
        s.customWidthMm = lo;
        s.customHeightMm = hi;
    }
    // The paper is stored unrotated; a quarter turn swaps its orientation.
    s.landscape = (r.paperWidthMm >= r.paperHeightMm) == (r.rotation % 2 == 0);
    if (r.plotType == 4 && r.windowMaxX > r.windowMinX && r.windowMaxY > r.windowMinY) {
        s.area = exportlib::PlotArea::Window;
        s.window.expand({r.windowMinX, r.windowMinY});
        s.window.expand({r.windowMaxX, r.windowMaxY});
    }
    s.fitToPaper = r.scaleType == 0 || !(r.scaleNum > 0) || !(r.scaleDen > 0);
    if (!s.fitToPaper)
        s.scaleN = r.scaleDen / r.scaleNum;
    s.centerOnPaper = r.centered;
    s.offsetMm = {r.originX, r.originY};
    s.lineweights = r.lineweights;
    std::string style = r.styleSheet;
    if (style.empty() || style == "None")
        style = "none";
    p.styleName = style;
    return p;
}

// PSTYLEMODE: colour-dependent (CTB) or named (STB) plot styles for this drawing.
class PStyleModeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PSTYLEMODE"; }
    std::string_view description() const override {
        return "Plot with colour-dependent or named plot styles";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool colour = colorDependentPlotStyles(ctx.doc().header());
        KeywordOptions ko;
        ko.keywords = {"Colour", "Named"};
        ko.defaultKeyword = colour ? "Colour" : "Named";
        auto r = co_await ctx.getKeyword(std::string("Plot styles [Colour-dependent/Named] <") +
                                             (colour ? "Colour" : "Named") + ">",
                                         ko);
        if (!r.ok())
            co_return;
        const bool wantColour = r.keyword == "Colour";
        if (wantColour == colour) {
            ctx.message(colour ? "Colour-dependent plot styles stay on."
                               : "Named plot styles stay on.");
            co_return;
        }
        (void)ctx.tx().modifyHeader([&](Header& h) { setColorDependentPlotStyles(h, wantColour); });
        ctx.setUndoLabel("PSTYLEMODE");
        ctx.message(wantColour ? "Plot styles follow the colour of each object."
                               : "Plot styles follow the style name of each layer or object. "
                                 "Choose a .stb table with PLOTSTYLE and give styles with "
                                 "ASSIGNPSTYLE.");
    }
};

// ASSIGNPSTYLE: gives a layer or objects the name of a plot style from a named (.stb) table.
class AssignPStyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ASSIGNPSTYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"APS"}; }
    std::string_view description() const override {
        return "Give a layer or objects a named plot style";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        KeywordOptions ko;
        ko.keywords = {"Layer", "Objects"};
        ko.defaultKeyword = "Layer";
        auto what = co_await ctx.getKeyword("Assign to [Layer/Objects] <Layer>", ko);
        if (!what.ok())
            co_return;
        std::vector<EntityId> ids;
        LayerId layer{};
        if (what.keyword == "Layer") {
            cmd::StringOptions lo;
            auto ln = co_await ctx.getString("Layer name", lo);
            if (!ln.ok())
                co_return;
            const Layer* l = ctx.doc().layerByName(ln.text);
            if (!l) {
                ctx.message("No layer named \"" + ln.text + "\".");
                co_return;
            }
            layer = l->id;
        } else {
            auto sel = co_await ctx.getSelection("Select objects");
            if (!sel.ok() || sel.ids.empty())
                co_return;
            ids = sel.ids;
        }
        // Names from the table in use help catching typing mistakes, but are not required: the
        // table can be chosen later.
        std::string known;
        if (auto t = resolveStyleTable(ctx.host(), plotStyleName(ctx.host()));
            t && t.value() && t.value()->named)
            for (const auto& st : t.value()->styles)
                known += (known.empty() ? "" : ", ") + st.name;
        cmd::StringOptions so;
        so.defaultValue = "Normal";
        auto name = co_await ctx.getString(
            known.empty() ? "Plot style name (ByLayer for objects to follow their layer)"
                          : "Plot style name [" + known + "]",
            so);
        if (!name.ok() || name.text.empty())
            co_return;
        (void)ctx.tx().modifyHeader([&](Header& h) {
            if (layer)
                setLayerPlotStyle(h, layer, name.text);
            for (EntityId id : ids)
                setEntityPlotStyle(h, id, name.text);
        });
        ctx.setUndoLabel("Plot style");
        ctx.message(std::string("Plot style \"") + name.text + "\" given to " +
                    (layer ? "the layer." : std::to_string(ids.size()) + " objects."));
        if (colorDependentPlotStyles(ctx.doc().header()))
            ctx.message("This drawing plots by colour; use PSTYLEMODE to plot by style name.");
    }
};

// PAGESETUP: named page setups saved in the drawing.
class PageSetupCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PAGESETUP"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
    std::string_view description() const override {
        return "Save, apply and assign named page setups (plot options kept in the drawing)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        KeywordOptions menu;
        menu.keywords = {"List", "Save", "Apply", "Assign", "Rename", "Delete", "Import"};
        menu.defaultKeyword = "List";
        auto choice = co_await ctx.getKeyword("Page setup", menu);
        if (!choice.ok())
            co_return;
        const std::string& kw = choice.keyword;
        if (kw == "List") {
            const auto all = layout::pageSetups(ctx.doc());
            if (all.empty())
                ctx.message("This drawing has no page setups. Use Save to create one.");
            const std::string model = layout::modelPageSetup(ctx.doc());
            for (const auto& p : all) {
                std::string line = "  " + p.name + ": " + p.settings.paperName +
                                   (p.settings.landscape ? " landscape" : " portrait") + ", " +
                                   scaleText(p.settings) + ", " +
                                   (p.styleName.empty() ? "no style" : p.styleName);
                std::string users;
                if (p.name == model)
                    users = "model";
                for (const auto& l : layout::layouts(ctx.doc()))
                    if (l.pageSetup == p.name)
                        users += (users.empty() ? "" : ", ") + l.name;
                if (!users.empty())
                    line += "  [" + users + "]";
                ctx.message(line);
            }
            co_return;
        }
        if (kw == "Import") {
            auto f = co_await ctx.getString("Drawing (.tbcad or .dxf) to take page setups from");
            if (!f.ok() || f.text.empty())
                co_return;
            std::vector<layout::PageSetup> found;
            if (lower(std::filesystem::path(f.text).extension().string()) == ".dxf") {
                std::ifstream in(platform::pathFromUtf8(f.text), std::ios::binary);
                if (!in) {
                    ctx.message("Cannot read " + f.text);
                    co_return;
                }
                std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
                dxf::Report report;
                auto other = dxf::importDxf(
                    {reinterpret_cast<const std::byte*>(raw.data()), raw.size()}, {}, report);
                if (!other) {
                    ctx.message(other.error().message);
                    co_return;
                }
                for (const auto& rec : other.value().summary.plotSetups)
                    found.push_back(pageSetupOf(rec));
                // The settings a layout carries become setups named after it, unless a named
                // setup already has that name.
                for (auto rec : other.value().summary.layoutSetups) {
                    if (rec.paperWidthMm <= 0 || rec.paperHeightMm <= 0)
                        continue;
                    const std::string layoutName = rec.name;
                    if (std::any_of(found.begin(), found.end(), [&](const layout::PageSetup& p) {
                            return lower(p.name) == lower(layoutName);
                        }))
                        continue;
                    found.push_back(pageSetupOf(rec));
                }
            } else {
                io::IoReport report;
                auto other = io::readTbcad(std::filesystem::path(f.text), report);
                if (!other) {
                    ctx.message(other.error().message);
                    co_return;
                }
                found = layout::pageSetups(*other.value());
            }
            std::size_t count = 0;
            for (const auto& p : found) {
                auto r = layout::savePageSetup(ctx.tx(), p);
                if (!r) {
                    ctx.message(p.name + ": " + r.error().message);
                    continue;
                }
                ++count;
            }
            ctx.message(count == 0 ? "That drawing has no page setups."
                                   : std::to_string(count) + " page setups imported.");
            co_return;
        }
        cmd::StringOptions so;
        if (kw == "Save") {
            so.defaultValue = layout::modelPageSetup(ctx.doc());
            auto n =
                co_await ctx.getString("Name for the page setup (the current plot options)", so);
            if (!n.ok() || n.text.empty())
                co_return;
            const auto p = currentPageSetup(ctx.host(), n.text);
            auto r = layout::savePageSetup(ctx.tx(), p);
            ctx.message(r ? "Page setup \"" + n.text + "\" saved." : r.error().message);
            co_return;
        }
        auto n = co_await ctx.getString(
            kw == "Assign" ? "Page setup to assign (empty = none)" : "Page setup name", so);
        if (!n.ok())
            co_return;
        if (kw == "Apply") {
            auto p = layout::findPageSetup(ctx.doc(), n.text);
            if (!p) {
                ctx.message("No page setup named \"" + n.text + "\".");
                co_return;
            }
            applyPageSetup(ctx.host(), *p);
            ctx.message("Page setup \"" + p->name + "\" is now the current plot options.");
        } else if (kw == "Assign") {
            cmd::StringOptions lo;
            lo.defaultValue = plotLayout(ctx.host()).empty() ? "Model" : plotLayout(ctx.host());
            auto l = co_await ctx.getString("Layout (Model = model space)", lo);
            if (!l.ok())
                co_return;
            const bool model = l.text.empty() || lower(l.text) == "model";
            auto r = model ? layout::assignModelPageSetup(ctx.tx(), n.text)
                           : layout::assignPageSetup(ctx.tx(), l.text, n.text);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.message(std::string(n.text.empty() ? "Page setup removed from "
                                                   : "Page setup assigned to ") +
                        (model ? "model space." : "layout \"" + l.text + "\"."));
        } else if (kw == "Rename") {
            auto nn = co_await ctx.getString("New name");
            if (!nn.ok())
                co_return;
            auto r = layout::renamePageSetup(ctx.tx(), n.text, nn.text);
            ctx.message(r ? "Renamed to \"" + nn.text + "\"." : r.error().message);
        } else if (kw == "Delete") {
            auto r = layout::deletePageSetup(ctx.tx(), n.text);
            ctx.message(r ? "Page setup \"" + n.text + "\" deleted." : r.error().message);
        }
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(
        std::make_unique<PlotCommand>("PLOT", std::vector<std::string>{"PRINT", "-PLOT"},
                                      "Plot the drawing to PDF, SVG, PNG or JPEG", Format::Pdf));
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTPDF", std::vector<std::string>{}, "Export the drawing to a PDF file", Format::Pdf));
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTSVG", std::vector<std::string>{}, "Export the drawing to an SVG file", Format::Svg));
    (void)registry.add(makePrinterCommand());
    (void)registry.add(std::make_unique<PStyleModeCommand>());
    (void)registry.add(std::make_unique<AssignPStyleCommand>());
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTPNG", std::vector<std::string>{}, "Export the drawing to a PNG image", Format::Png));
    (void)registry.add(
        std::make_unique<PlotCommand>("EXPORTJPG", std::vector<std::string>{"EXPORTJPEG"},
                                      "Export the drawing to a JPEG image", Format::Jpeg));
    (void)registry.add(std::make_unique<PlotPanelCommand>());
    (void)registry.add(std::make_unique<PlotSetCommand>());
    (void)registry.add(std::make_unique<PublishCommand>());
    (void)registry.add(std::make_unique<PlotStampCommand>());
    (void)registry.add(makeETransmitCommand());
    (void)registry.add(makeCompareCommand());
    (void)registry.add(makePlotFrameCommand());
    (void)registry.add(std::make_unique<PlotStyleCommand>());
    (void)registry.add(std::make_unique<PageSetupCommand>());
    (void)registry.add(std::make_unique<PageSetupPanelCommand>());
    (void)registry.add(std::make_unique<PreviewCommand>());
}

}  // namespace tbcad::plot
