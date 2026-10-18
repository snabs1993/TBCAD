#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>

#include "tbcad/plot/plot.h"
#include "tbcad/sheets/sheets.h"

namespace tbcad::plot {

std::unique_ptr<cmd::Command> makePrinterCommand();

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
    t += s.area == PlotArea::Extents   ? ", Extents"
         : s.area == PlotArea::Display ? ", Display"
                                       : ", Window";
    t += ", scale " + scaleText(s);
    t += ", " + styleLabel(s);
    return t;
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
        PlotSettings s = loadSettings(ctx.host());
        Format format = format_;
        for (;;) {
            KeywordOptions menu;
            menu.keywords = {"Paper",  "Orientation", "Area",        "Scale", "Center",
                             "Offset", "Margin",      "LineWeights", "Color", "Style",
                             "Format", "Resolution",  "Layout",      "Stamp", "Export"};
            menu.defaultKeyword = "Export";
            auto choice =
                co_await ctx.getKeyword("Plot " + summary(ctx.doc(), ctx.host(), s, format), menu);
            if (choice.status == Status::Cancel)
                co_return;
            const std::string& kw = choice.keyword;

            if (kw == "Export") {
                if (!s.styleTable && styleTableMissing(ctx.host())) {
                    KeywordOptions yn;
                    yn.keywords = {"Yes", "No"};
                    yn.defaultKeyword = "Yes";
                    ctx.message("Plot style table \"" + plotStyleName(ctx.host()) +
                                "\" was not found.");
                    auto use =
                        co_await ctx.getKeyword("Plot in black with monochrome.ctb instead", yn);
                    if (use.status == Status::Cancel)
                        co_return;
                    if (use.ok() && use.keyword == "Yes") {
                        setPlotStyleName(ctx.host(), plotstyle::kDefaultTable);
                        s = loadSettings(ctx.host());
                    }
                }
                cmd::StringOptions fo;
                fo.defaultValue = defaultFile(ctx.host(), format);
                auto file = co_await ctx.getString("Output file", fo);
                if (!file.ok())
                    co_return;
                auto r = exportFile(ctx.doc(), ctx.host(), s, format, file.text);
                if (!r) {
                    ctx.message(r.error().message);
                    continue;  // let the user fix the settings and try again
                }
                saveSettings(ctx.host(), s);
                setLastFile(ctx.host(), format, withExtension(file.text, format));
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
                        ctx.message("Unknown paper size \"" + name.text + "\".");
                        continue;
                    }
                    s.paperName = found->name;
                }
            } else if (kw == "Orientation") {
                KeywordOptions ko;
                ko.keywords = {"Landscape", "Portrait"};
                ko.defaultKeyword = s.landscape ? "Landscape" : "Portrait";
                auto r = co_await ctx.getKeyword("Orientation", ko);
                if (r.ok())
                    s.landscape = r.keyword == "Landscape";
            } else if (kw == "Area") {
                KeywordOptions ko;
                ko.keywords = {"Extents", "Display", "Window"};
                ko.defaultKeyword = s.area == PlotArea::Extents   ? "Extents"
                                    : s.area == PlotArea::Display ? "Display"
                                                                  : "Window";
                auto r = co_await ctx.getKeyword("Plot area", ko);
                if (!r.ok())
                    continue;
                if (r.keyword == "Window") {
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
                } else {
                    s.area = r.keyword == "Extents" ? PlotArea::Extents : PlotArea::Display;
                }
            } else if (kw == "Scale") {
                cmd::StringOptions so;
                so.defaultValue = scaleText(s);
                auto r = co_await ctx.getString("Scale (Fit or 1:N, for example 1:100)", so);
                if (r.ok() && !parseScale(r.text, s))
                    ctx.message("Not a scale: \"" + r.text + "\". Use Fit or 1:N.");
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
            } else if (kw == "Color") {
                KeywordOptions ko;
                ko.keywords = {"Color", "Monochrome", "Greyscale"};
                ko.defaultKeyword = s.color == ColorMode::Monochrome  ? "Monochrome"
                                    : s.color == ColorMode::Greyscale ? "Greyscale"
                                                                      : "Color";
                auto r = co_await ctx.getKeyword("Colour mode", ko);
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
                    ctx.message(t.error().message);
                    continue;
                }
                setPlotStyleName(ctx.host(), r.text);
                s.styleTable = t.value();
            } else if (kw == "Layout") {
                cmd::StringOptions so;
                so.defaultValue = plotLayout(ctx.host());
                auto r = co_await ctx.getString("Layout to plot (empty = model space)", so);
                if (r.ok()) {
                    setPlotLayout(ctx.host(), r.text);
                    if (const auto m = applyAssignedPageSetup(ctx.doc(), ctx.host()); !m.empty()) {
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

// PLOTSET: plots every sheet of a sheet list (see tbcad/sheets/sheets.h) into one PDF.
class PlotSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTSET"; }
    std::vector<std::string> defaultAliases() const override { return {"BATCHPLOT", "PUBLISH"}; }
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
                                              styleResolver(ctx.host()));
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
        auto r = sheets::plotSheetList(entries.value(), loadSettings(ctx.host()), out,
                                       styleResolver(ctx.host()));
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        for (const auto& w : r.value().warnings)
            ctx.message("Warning: " + w);
        ctx.message("Plotted " + std::to_string(r.value().pages) + " sheets to " + out);
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
        menu.keywords = {"Select", "List", "Import", "Edit", "Save"};
        menu.defaultKeyword = "List";
        auto choice =
            co_await ctx.getKeyword("Plot style table: " + plotStyleName(ctx.host()), menu);
        if (!choice.ok())
            co_return;
        const std::string& kw = choice.keyword;
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
            auto file = co_await ctx.getString("Path of the .ctb file to import", so);
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
        menu.keywords = {"List", "Save", "Apply", "Assign", "Rename", "Delete"};
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
    (void)registry.add(std::make_unique<PlotCommand>("PLOT", std::vector<std::string>{"PRINT"},
                                                     "Plot the drawing to PDF, SVG, PNG or JPEG",
                                                     Format::Pdf));
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTPDF", std::vector<std::string>{}, "Export the drawing to a PDF file", Format::Pdf));
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTSVG", std::vector<std::string>{}, "Export the drawing to an SVG file", Format::Svg));
    (void)registry.add(makePrinterCommand());
    (void)registry.add(std::make_unique<PlotCommand>(
        "EXPORTPNG", std::vector<std::string>{}, "Export the drawing to a PNG image", Format::Png));
    (void)registry.add(
        std::make_unique<PlotCommand>("EXPORTJPG", std::vector<std::string>{"EXPORTJPEG"},
                                      "Export the drawing to a JPEG image", Format::Jpeg));
    (void)registry.add(std::make_unique<PlotPanelCommand>());
    (void)registry.add(std::make_unique<PlotSetCommand>());
    (void)registry.add(std::make_unique<PlotStyleCommand>());
    (void)registry.add(std::make_unique<PageSetupCommand>());
}

}  // namespace tbcad::plot
