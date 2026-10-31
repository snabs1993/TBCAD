// PLOTFRAME: named plot frames in model space (layout/plotframes.h): paper, orientation, scale and
// position of each sheet, so many model space sheets at different scales plot in one go.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "tbcad/geom/entity.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

using cmd::KeywordOptions;

std::string lowerText(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Vec2> ring(const BBox2& b) {
    return {b.min, {b.max.x, b.min.y}, b.max, {b.min.x, b.max.y}};
}

// The paper, orientation and scale of a new frame, asked in turn. False when cancelled.
cmd::Task askPaperAndScale(cmd::CommandContext& ctx, layout::PlotFrame& f, bool& ok) {
    ok = false;
    for (;;) {
        cmd::StringOptions po;
        po.defaultValue = f.paperName;
        auto paper = co_await ctx.getString("Paper size (A0-A4, ANSI A-E)", po);
        if (!paper.ok())
            co_return;
        const exportlib::PaperSize* found = nullptr;
        for (const auto& p : exportlib::paperSizes())
            if (lowerText(p.name) == lowerText(paper.text))
                found = &p;
        if (found) {
            f.paperName = found->name;
            break;
        }
        ctx.message("Unknown paper size \"" + paper.text + "\".");
    }
    KeywordOptions ko;
    ko.keywords = {"Landscape", "Portrait"};
    ko.defaultKeyword = f.landscape ? "Landscape" : "Portrait";
    auto orient = co_await ctx.getKeyword("Orientation", ko);
    if (orient.status == cmd::Status::Cancel)
        co_return;
    if (orient.ok())
        f.landscape = orient.keyword == "Landscape";
    for (;;) {
        cmd::StringOptions so;
        char def[32];
        std::snprintf(def, sizeof def, "1:%.4g", f.scaleN);
        so.defaultValue = def;
        auto sc = co_await ctx.getString("Scale (1:N, for example 1:100)", so);
        if (!sc.ok())
            co_return;
        exportlib::PlotSettings tmp;
        if (parseScale(sc.text, tmp) && !tmp.fitToPaper && tmp.scaleN > 0) {
            f.scaleN = tmp.scaleN;
            f.fitToPaper = false;
            break;
        }
        ctx.message("Not a scale: \"" + sc.text + "\". Use 1:N.");
    }
    ok = true;
}

// Frame names typed as numbers or names ("1,5,7-9"); asked again after a bad entry, empty when
// cancelled.
cmd::Task askFrames(cmd::CommandContext& ctx, const char* prompt, std::vector<std::string>& out) {
    out.clear();
    for (;;) {
        auto t = co_await ctx.getString(prompt);
        if (!t.ok() || t.text.empty())
            co_return;
        auto names = layout::resolveFrameList(ctx.doc(), t.text);
        if (names) {
            out = std::move(names.value());
            co_return;
        }
        ctx.message(names.error().message);
    }
}

// Frames sorted the way sheets are read: rows from the top, left to right in a row.
void sortReadingOrder(std::vector<BBox2>& boxes) {
    std::sort(boxes.begin(), boxes.end(), [](const BBox2& a, const BBox2& b) {
        const double tol = 0.5 * std::min(a.height(), b.height());
        if (std::abs(a.max.y - b.max.y) > tol)
            return a.max.y > b.max.y;
        return a.min.x < b.min.x;
    });
}

class PlotFrameCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PLOTFRAME"; }
    std::vector<std::string> defaultAliases() const override { return {"PFRAME"}; }
    std::string_view description() const override {
        return "Named plot frames in model space: paper, scale and position for plotting many "
               "model space sheets at once";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("PLOTFRAME");
        KeywordOptions menu;
        menu.keywords = {"New", "Object", "Window", "List", "Rename", "Delete", "Plot", "All"};
        menu.defaultKeyword = layout::plotFrames(ctx.doc()).empty() ? "New" : "List";
        auto choice = co_await ctx.getKeyword("Plot frames", menu);
        if (!choice.ok())
            co_return;
        const std::string kw = choice.keyword;
        if (kw == "List") {
            const auto frames = layout::plotFrames(ctx.doc());
            if (frames.empty())
                ctx.message("This drawing has no plot frames. Use New to add one.");
            int n = 0;
            for (const auto& f : frames) {
                char size[64];
                std::snprintf(size, sizeof size, "%.4g x %.4g", f.window.width(),
                              f.window.height());
                ctx.message("  " + std::to_string(++n) + ". " + f.name + ": " +
                            layout::describeFrame(f) + ", area " + size +
                            (f.pageSetup.empty() ? "" : ", page setup " + f.pageSetup));
            }
            co_return;
        }
        if (kw == "Rename" || kw == "Delete") {
            auto n = co_await ctx.getString("Plot frame name");
            if (!n.ok() || n.text.empty())
                co_return;
            if (kw == "Delete") {
                auto r = layout::deletePlotFrame(ctx.tx(), n.text);
                ctx.message(r ? "Plot frame \"" + n.text + "\" deleted." : r.error().message);
                co_return;
            }
            auto nn = co_await ctx.getString("New name");
            if (!nn.ok())
                co_return;
            auto r = layout::renamePlotFrame(ctx.tx(), n.text, nn.text);
            ctx.message(r ? "Renamed to \"" + nn.text + "\"." : r.error().message);
            co_return;
        }
        if (kw == "Plot" || kw == "All") {
            std::vector<std::string> names;
            if (kw == "Plot") {
                auto n = co_await ctx.getString(
                    "Plot frames to plot (numbers or names, for example 1,5,7-9)");
                if (!n.ok() || n.text.empty())
                    co_return;
                auto resolved = layout::resolveFrameList(ctx.doc(), n.text);
                if (!resolved) {
                    ctx.message(resolved.error().message);
                    co_return;
                }
                names = std::move(resolved.value());
            } else {
                for (const auto& f : layout::plotFrames(ctx.doc()))
                    names.push_back(f.name);
            }
            if (names.empty()) {
                ctx.message("This drawing has no plot frames.");
                co_return;
            }
            cmd::StringOptions fo;
            std::string def = defaultFile(ctx.host(), Format::Pdf);
            if (names.size() == 1) {
                const auto dot = def.rfind('.');
                def = (dot == std::string::npos ? def : def.substr(0, dot)) + " " + names.front() +
                      ".pdf";
            }
            fo.defaultValue = def;
            auto file = co_await ctx.getString("Output PDF file (or Printer / Dialog)", fo);
            if (!file.ok() || file.text.empty())
                co_return;
            auto r = plotFramesTo(ctx.doc(), ctx.host(), names, file.text);
            ctx.message(r ? r.value() : r.error().message);
            co_return;
        }

        // New, Object, Window: the sheet's paper and scale first.
        layout::PlotFrame f;
        const exportlib::PlotSettings cur = loadSettings(ctx.host());
        f.paperName = cur.paperName == "Custom" ? "A3" : cur.paperName;
        f.landscape = cur.landscape;
        f.scaleN = cur.fitToPaper || !(cur.scaleN > 0) ? 100 : cur.scaleN;
        if (const auto frames = layout::plotFrames(ctx.doc()); !frames.empty()) {
            f.paperName = frames.back().paperName;
            f.landscape = frames.back().landscape;
            f.scaleN = frames.back().scaleN;
        }
        std::vector<BBox2> boxes;
        if (kw == "Object") {
            cmd::SelectionOptions so;
            auto sel = co_await ctx.getSelection(
                "Select the sheet borders or title blocks (one frame each)", so);
            if (!sel.ok() || sel.ids.empty())
                co_return;
            for (EntityId id : sel.ids)
                if (const Entity* e = ctx.doc().find(id)) {
                    const BBox2 b = geom::bounds(*e, ctx.doc());
                    if (!b.empty() && b.width() > 0 && b.height() > 0)
                        boxes.push_back(b);
                }
            if (boxes.empty()) {
                ctx.message("None of the selected objects has an area.");
                co_return;
            }
            sortReadingOrder(boxes);
        }
        bool ok = false;
        co_await askPaperAndScale(ctx, f, ok);
        if (!ok)
            co_return;
        if (kw == "New") {
            const double margin = cur.marginMm;
            const layout::PlotFrame shape = f;
            cmd::PointOptions po;
            po.preview = [shape, margin](Vec2 cursor, DisplayList& out) {
                const BBox2 b = layout::frameBoxForPaper(shape.paperName, shape.landscape, margin,
                                                         shape.scaleN, cursor);
                if (b.empty())
                    return;
                DisplayPolyline pl;
                pl.pts = ring(b);
                pl.closed = true;
                out.items.emplace_back(std::move(pl));
            };
            auto p = co_await ctx.getPoint("Lower-left corner of the sheet", po);
            if (!p.ok())
                co_return;
            boxes.push_back(
                layout::frameBoxForPaper(f.paperName, f.landscape, margin, f.scaleN, p.point));
        } else if (kw == "Window") {
            auto a = co_await ctx.getPoint("Specify first corner");
            if (!a.ok())
                co_return;
            cmd::PointOptions po;
            po.base = a.point;
            po.preview = [from = a.point](Vec2 cursor, DisplayList& out) {
                BBox2 b;
                b.expand(from);
                b.expand(cursor);
                DisplayPolyline pl;
                pl.pts = ring(b);
                pl.closed = true;
                out.items.emplace_back(std::move(pl));
            };
            auto b = co_await ctx.getPoint("Specify opposite corner", po);
            if (!b.ok())
                co_return;
            BBox2 w;
            w.expand(a.point);
            w.expand(b.point);
            boxes.push_back(w);
        }
        std::string name;
        if (boxes.size() == 1) {
            cmd::StringOptions no;
            const std::string defName = layout::nextPlotFrameName(ctx.doc());
            no.defaultValue = defName;
            for (;;) {
                auto n = co_await ctx.getString("Name of the plot frame", no);
                if (!n.ok())
                    co_return;
                name = n.text.empty() ? defName : n.text;
                auto valid = layout::validatePlotFrameName(ctx.doc(), name);
                if (valid)
                    break;
                ctx.message(valid.error().message);
            }
        }
        std::vector<std::string> added;
        for (const BBox2& b : boxes) {
            f.window = b;
            f.name = boxes.size() == 1 ? name : layout::nextPlotFrameName(ctx.doc());
            auto r = layout::savePlotFrame(ctx.tx(), f);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            added.push_back(f.name);
        }
        ctx.message(added.size() == 1
                        ? "Plot frame \"" + added.front() + "\" added (" +
                              layout::describeFrame(f) + ")."
                        : std::to_string(added.size()) + " plot frames added (" + added.front() +
                              " to " + added.back() + ", " + layout::describeFrame(f) + ").");
    }
};

// FRAMESET: named sets of plot frames ("Issue 1": sheets 1, 5 and 7), saved in the drawing.
class FrameSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FRAMESET"; }
    std::vector<std::string> defaultAliases() const override { return {"PLOTSETS"}; }
    std::string_view description() const override {
        return "Named sets of plot frames (for example sheets 1, 5 and 7): save, copy, add or "
               "remove sheets and plot only the set";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("FRAMESET");
        if (layout::plotFrames(ctx.doc()).empty()) {
            ctx.message("This drawing has no plot frames yet. Add them with PLOTFRAME first.");
            co_return;
        }
        KeywordOptions menu;
        menu.keywords = {"New", "Add", "Remove", "Copy", "Rename", "Delete", "List", "Plot"};
        menu.defaultKeyword = layout::plotSets(ctx.doc()).empty() ? "New" : "Plot";
        auto choice = co_await ctx.getKeyword("Plot sets", menu);
        if (!choice.ok())
            co_return;
        const std::string kw = choice.keyword;
        if (kw == "List") {
            const auto sets = layout::plotSets(ctx.doc());
            if (sets.empty())
                ctx.message("This drawing has no plot sets. Use New to make one.");
            for (const auto& p : sets) {
                std::string line = "  " + p.name + " (" + std::to_string(p.frames.size()) + "): ";
                for (std::size_t i = 0; i < p.frames.size(); ++i)
                    line += (i ? ", " : "") + p.frames[i];
                ctx.message(line);
            }
            co_return;
        }
        std::string setName;
        if (kw == "New") {
            cmd::StringOptions no;
            const std::string def = layout::nextPlotSetName(ctx.doc());
            no.defaultValue = def;
            auto n = co_await ctx.getString("Name of the plot set", no);
            if (!n.ok())
                co_return;
            setName = n.text.empty() ? def : n.text;
            // An existing set's name replaces its sheets (the panel saves checked sheets so).
            const bool replacing = layout::findPlotSet(ctx.doc(), setName).has_value();
            if (auto ok =
                    replacing ? Result<void>{} : layout::validatePlotSetName(ctx.doc(), setName);
                !ok) {
                ctx.message(ok.error().message);
                co_return;
            }
            std::vector<std::string> frames;
            co_await askFrames(ctx, "Frames in the set (numbers or names, for example 1,5,7-9)",
                               frames);
            if (frames.empty())
                co_return;
            auto r = layout::savePlotSet(ctx.tx(), {setName, frames});
            ctx.message(r ? "Plot set \"" + setName + "\" saved with " +
                                std::to_string(frames.size()) + " frames."
                          : r.error().message);
            co_return;
        }
        {
            cmd::StringOptions so;
            if (const auto sets = layout::plotSets(ctx.doc()); !sets.empty())
                so.defaultValue = sets.front().name;
            auto n = co_await ctx.getString("Plot set", so);
            if (!n.ok() || n.text.empty())
                co_return;
            auto found = layout::findPlotSet(ctx.doc(), n.text);
            if (!found) {
                ctx.message("No plot set named \"" + n.text + "\".");
                co_return;
            }
            setName = found->name;
        }
        if (kw == "Add" || kw == "Remove") {
            std::vector<std::string> frames;
            co_await askFrames(ctx,
                               kw == "Add" ? "Frames to add (numbers or names)"
                                           : "Frames to take out (numbers or names)",
                               frames);
            if (frames.empty())
                co_return;
            auto r = kw == "Add" ? layout::addToPlotSet(ctx.tx(), setName, frames)
                                 : layout::removeFromPlotSet(ctx.tx(), setName, frames);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.message("Plot set \"" + setName + "\" now has " +
                        std::to_string(layout::findPlotSet(ctx.doc(), setName)->frames.size()) +
                        " frames.");
        } else if (kw == "Copy" || kw == "Rename") {
            cmd::StringOptions no;
            if (kw == "Copy")
                no.defaultValue = setName + " copy";
            auto nn = co_await ctx.getString("New name", no);
            if (!nn.ok())
                co_return;
            const std::string newName =
                nn.text.empty() && kw == "Copy" ? setName + " copy" : nn.text;
            auto r = kw == "Copy" ? layout::copyPlotSet(ctx.tx(), setName, newName)
                                  : layout::renamePlotSet(ctx.tx(), setName, newName);
            ctx.message(r ? (kw == "Copy" ? "Copied to \"" : "Renamed to \"") + newName + "\"."
                          : r.error().message);
        } else if (kw == "Delete") {
            auto r = layout::deletePlotSet(ctx.tx(), setName);
            ctx.message(r ? "Plot set \"" + setName + "\" deleted." : r.error().message);
        } else if (kw == "Plot") {
            const auto set = layout::findPlotSet(ctx.doc(), setName);
            if (set->frames.empty()) {
                ctx.message("Plot set \"" + setName + "\" is empty.");
                co_return;
            }
            cmd::StringOptions fo;
            std::string def = defaultFile(ctx.host(), Format::Pdf);
            const auto dot = def.rfind('.');
            fo.defaultValue =
                (dot == std::string::npos ? def : def.substr(0, dot)) + " " + setName + ".pdf";
            auto file = co_await ctx.getString("Output PDF file (or Printer / Dialog)", fo);
            if (!file.ok() || file.text.empty())
                co_return;
            auto r = plotFramesTo(ctx.doc(), ctx.host(), set->frames, file.text);
            ctx.message(r ? r.value() : r.error().message);
        }
    }
};

}  // namespace

Result<std::vector<exportlib::PlotSheet>> buildFrameSheets(const Document& doc,
                                                           const cmd::IHost& host,
                                                           const std::vector<std::string>& names) {
    const exportlib::PlotSettings base = loadSettings(host);
    const layout::StyleResolver styles = styleResolver(host);
    std::vector<exportlib::PlotSheet> sheets;
    for (const std::string& n : names) {
        auto f = layout::findPlotFrame(doc, n);
        if (!f)
            return Error{ErrorCode::NotFound, "No plot frame named \"" + n + "\""};
        const exportlib::PlotSettings s = layout::frameSettings(doc, base, *f, styles);
        auto sheet = exportlib::buildSheet(doc, s);
        if (!sheet)
            return Error{sheet.error().code, f->name + ": " + sheet.error().message};
        addPlotStamp(sheet.value(), host, s);
        if (s.upsideDown)
            exportlib::rotateSheet180(sheet.value());
        if (!pdfLayersOn(host))
            exportlib::dropLayers(sheet.value());
        sheets.push_back(std::move(sheet.value()));
    }
    return sheets;
}

Result<std::string> plotFramesTo(const Document& doc, const cmd::IHost& host,
                                 const std::vector<std::string>& names, const std::string& file) {
    auto sheets = buildFrameSheets(doc, host, names);
    if (!sheets)
        return sheets.error();
    std::string warn;
    for (std::size_t i = 0; i < sheets.value().size(); ++i)
        if (sheets.value()[i].layout.overflows)
            warn += " Warning: " + names[i] + " is larger than its paper and was clipped.";
    const std::string lf = lowerText(file);
    if (lf == "printer" || lf == "dialog") {
        if (!print::available())
            return Error{ErrorCode::Unsupported,
                         "Printing is only available in the Windows version. Plot to a PDF "
                         "instead."};
        auto r = print::printSheets(sheets.value(), printJob(host, lf == "dialog"));
        if (!r)
            return r.error();
        return "Sent " + std::to_string(sheets.value().size()) + " plot frames to the printer." +
               warn;
    }
    const std::string out = withExtension(file, Format::Pdf);
    if (auto parent = platform::pathFromUtf8(out).parent_path(); !parent.empty())
        platform::ensureDirectory(parent);
    auto w = exportlib::writePdfPages(sheets.value(), out);
    if (!w)
        return w.error();
    return "Plotted " + std::to_string(sheets.value().size()) + " plot frames to " + out + "." +
           warn;
}

std::unique_ptr<cmd::Command> makePlotFrameCommand() {
    return std::make_unique<PlotFrameCommand>();
}

std::unique_ptr<cmd::Command> makeFrameSetCommand() {
    return std::make_unique<FrameSetCommand>();
}

}  // namespace tbcad::plot
