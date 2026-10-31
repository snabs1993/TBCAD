#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "panel.h"
#include "tbcad/layout/layout.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

using exportlib::ColorMode;
using exportlib::PlotArea;
using exportlib::PlotSettings;

struct PanelState {
    PlotSettings settings{};
    bool loaded = false;
    std::string scaleInput{};
    std::string fileInput{};
    Format format = Format::Pdf;
    std::string message{};
    // Preview cache: rebuilt when the document, the settings or the view-dependent window change.
    exportlib::PlotSheet sheet{};
    std::string sheetError{};
    std::uint64_t builtRevision = ~0ull;
    std::string builtKey{};
    std::string styleLoaded{};
    std::string setupName{};         // text field of "Save as"
    std::string confirmOverwrite{};  // file the user was warned about; a second click replaces it
    bool refreshPreview = false;     // big drawing: the user asked for a new preview
    bool reloadWhenIdle = false;     // a command changed the host settings; read them again
};

PanelState& state() {
    static PanelState s;
    return s;
}

std::string keyOf(const PlotSettings& s, const BBox2& displayWindow) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s|%g|%g|%d|%d|%g,%g,%g,%g|%d|%g|%d|%g,%g|%g|%d|%d|%g,%g,%g,%g|%d",
                  s.paperName.c_str(), s.customWidthMm, s.customHeightMm, s.landscape, int(s.area),
                  s.window.min.x, s.window.min.y, s.window.max.x, s.window.max.y, s.fitToPaper,
                  s.scaleN, s.centerOnPaper, s.offsetMm.x, s.offsetMm.y, s.marginMm, s.lineweights,
                  int(s.color), displayWindow.min.x, displayWindow.min.y, displayWindow.max.x,
                  displayWindow.max.y, s.transparency);
    return buf;
}

ImU32 col(Rgba c) {
    return IM_COL32(c.r, c.g, c.b, 255);
}

// Drawings with more objects than this get a preview that is built on request, and the preview
// draws a sample of the primitives: building the sheet takes seconds and drawing every primitive
// of a few hundred thousand objects would cost a frame each.
constexpr std::size_t kBigDrawing = 50000;
constexpr std::size_t kPreviewPrims = 30000;
// The renderer may use 16-bit indices: the preview stops adding primitives at this many vertices
// in the window's draw list.
constexpr int kPreviewVertices = 55000;

void drawPreview(const exportlib::PlotSheet& sheet, ImVec2 size) {
    const auto& lay = sheet.layout;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Fit the page into the available rectangle, leaving room for a border.
    const float pad = 8;
    float k = std::min((size.x - 2 * pad) / float(lay.pageWidthMm),
                       (size.y - 2 * pad) / float(lay.pageHeightMm));
    k = std::max(k, 0.01f);
    float pw = float(lay.pageWidthMm) * k, ph = float(lay.pageHeightMm) * k;
    ImVec2 p0{origin.x + (size.x - pw) / 2, origin.y + (size.y - ph) / 2};
    auto toScreen = [&](Vec2 p) {
        return ImVec2(p0.x + float(p.x) * k, p0.y + ph - float(p.y) * k);
    };

    dl->AddRectFilled({p0.x + 3, p0.y + 3}, {p0.x + pw + 3, p0.y + ph + 3}, IM_COL32(0, 0, 0, 60));
    dl->AddRectFilled(p0, {p0.x + pw, p0.y + ph}, IM_COL32(255, 255, 255, 255));
    ImVec2 pa = toScreen(lay.printable.min), pb = toScreen(lay.printable.max);
    dl->AddRect({pa.x, pb.y}, {pb.x, pa.y}, IM_COL32(120, 160, 220, 160));
    dl->PushClipRect({pa.x, pb.y}, {pb.x, pa.y}, true);

    // Same primitives as the exported file; line widths use the real paper width, at least 1 px.
    std::vector<ImVec2> pts;
    const std::size_t step = sheet.prims.size() > kPreviewPrims
                                 ? (sheet.prims.size() + kPreviewPrims - 1) / kPreviewPrims
                                 : 1;
    for (std::size_t pi = 0; pi < sheet.prims.size(); pi += step) {
        if (dl->VtxBuffer.Size > kPreviewVertices)
            break;
        const auto& prim = sheet.prims[pi];
        if (const auto* p = std::get_if<exportlib::PlotPath>(&prim)) {
            pts.clear();
            for (const Vec2& v : p->pts)
                pts.push_back(toScreen(v));
            dl->AddPolyline(pts.data(), int(pts.size()), col(p->style.color),
                            p->closed ? ImDrawFlags_Closed : ImDrawFlags_None,
                            std::max(1.0f, float(p->style.widthMm) * k));
        } else if (const auto* a = std::get_if<exportlib::PlotArc>(&prim)) {
            ImVec2 c = toScreen(a->center);
            float r = float(a->radius) * k;
            float w = std::max(1.0f, float(a->style.widthMm) * k);
            if (a->full) {
                dl->AddCircle(c, r, col(a->style.color), 0, w);
            } else {
                // Screen y runs downwards: negate the angles.
                dl->PathArcTo(c, r, float(-a->end), float(-a->start), 0);
                dl->PathStroke(col(a->style.color), ImDrawFlags_None, w);
            }
        } else if (const auto* f = std::get_if<exportlib::PlotFill>(&prim)) {
            // The preview outlines fills; the file is filled even-odd.
            for (const auto& ring : f->rings) {
                pts.clear();
                for (const Vec2& v : ring)
                    pts.push_back(toScreen(v));
                dl->AddPolyline(pts.data(), int(pts.size()), col(f->color), ImDrawFlags_Closed,
                                1.0f);
            }
        } else if (const auto* d = std::get_if<exportlib::PlotDots>(&prim)) {
            for (const Vec2& v : d->pts)
                dl->AddCircleFilled(toScreen(v), std::max(1.0f, float(d->style.widthMm) * k / 2),
                                    col(d->style.color));
        }
    }
    dl->PopClipRect();
    ImGui::Dummy(size);
}

void draw(ui::PanelContext& pc) {
    PanelState& st = state();
    if (!pc.doc || !pc.host)
        return;
    // Opened by Modify in the Page Setup Manager: show that setup's options, ready to save back.
    if (auto n = pc.host->setting("tmp.plot.setupName"); n && !n->empty()) {
        st.setupName = *n;
        pc.host->setSetting("tmp.plot.setupName", "");
        st.reloadWhenIdle = true;
    }
    if (st.reloadWhenIdle && !pc.commandActive) {
        st.loaded = false;
        st.reloadWhenIdle = false;
        st.styleLoaded.clear();
    }
    if (!st.loaded) {
        if (!pc.commandActive)
            if (std::string m = followPlottedLayoutSetup(*pc.doc, *pc.host); !m.empty()) {
                st.message = std::move(m);
                st.styleLoaded.clear();
            }
        st.settings = loadSettings(*pc.host);
        st.scaleInput = scaleText(st.settings);
        st.fileInput = defaultFile(*pc.host, st.format);
        st.loaded = true;
    }
    PlotSettings& s = st.settings;
    bool changed = false;

    // What to plot: model space, or one of the drawing's layouts (which brings its own paper).
    const std::string layoutName = plotLayout(*pc.host);
    if (ImGui::BeginCombo("Plot", layoutName.empty() ? "Model" : layoutName.c_str())) {
        bool picked = false;
        if (ImGui::Selectable("Model", layoutName.empty())) {
            setPlotLayout(*pc.host, "");
            picked = true;
        }
        for (const auto& l : layout::layouts(*pc.doc))
            if (ImGui::Selectable(l.name.c_str(), l.name == layoutName)) {
                setPlotLayout(*pc.host, l.name);
                picked = true;
            }
        ImGui::EndCombo();
        if (picked) {  // a layout with a page setup brings its options along
            pc.host->setSetting("tmp.plot.setupFollowed", "");
            st.message = followPlottedLayoutSetup(*pc.doc, *pc.host);
            st.loaded = false;
            st.styleLoaded.clear();
            return;
        }
    }

    // Page setups saved in the drawing.
    {
        const auto setups = layout::pageSetups(*pc.doc);
        const auto assigned = layoutName.empty() ? layout::pageSetupOfModel(*pc.doc)
                                                 : layout::pageSetupOfLayout(*pc.doc, layoutName);
        if (ImGui::BeginCombo("Page setup", assigned ? assigned->name.c_str() : "<none>")) {
            for (const auto& p : setups)
                if (ImGui::Selectable(p.name.c_str(), assigned && assigned->name == p.name)) {
                    pc.runCommand("PAGESETUP");
                    pc.runCommand("Apply");
                    pc.runCommand(p.name);
                    pc.runCommand("PAGESETUP");
                    pc.runCommand("Assign");
                    pc.runCommand(p.name);
                    pc.runCommand("");  // the layout being plotted
                    st.reloadWhenIdle = true;
                }
            if (ImGui::Selectable("<none>", !assigned) && assigned) {
                pc.runCommand("PAGESETUP");
                pc.runCommand("Assign");
                pc.runCommand("");
                pc.runCommand("");
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("Save current options as", &st.setupName);
        ImGui::SameLine();
        if (ImGui::Button("Save") && !st.setupName.empty()) {
            saveSettings(*pc.host, st.settings);
            pc.runCommand("PAGESETUP");
            pc.runCommand("Save");
            pc.runCommand(st.setupName);
        }
    }

    // Plot style table.
    const std::string styleName = plotStyleName(*pc.host);
    if (ImGui::BeginCombo("Style", styleName.c_str())) {
        for (const auto& n : listStyleTables(*pc.host))
            if (ImGui::Selectable(n.c_str(), n == styleName))
                setPlotStyleName(*pc.host, n);
        if (ImGui::Selectable("none", false))
            setPlotStyleName(*pc.host, "none");
        ImGui::EndCombo();
    }
    if (st.styleLoaded != styleName) {
        auto t = resolveStyleTable(*pc.host, styleName);
        s.styleTable = t ? t.value() : nullptr;
        st.styleLoaded = styleName;
    }

    // Paper
    if (ImGui::BeginCombo("Paper", s.paperName.c_str())) {
        for (const auto& p : exportlib::paperSizes())
            if (ImGui::Selectable(p.name.c_str(), p.name == s.paperName)) {
                s.paperName = p.name;
                changed = true;
            }
        if (ImGui::Selectable("Custom", s.paperName == "Custom")) {
            s.paperName = "Custom";
            changed = true;
        }
        ImGui::EndCombo();
    }
    if (s.paperName == "Custom") {
        changed |= ImGui::InputDouble("Width (mm)", &s.customWidthMm, 1, 10, "%.1f",
                                      ImGuiInputTextFlags_AutoSelectAll);
        changed |= ImGui::InputDouble("Height (mm)", &s.customHeightMm, 1, 10, "%.1f",
                                      ImGuiInputTextFlags_AutoSelectAll);
        s.customWidthMm = std::max(1.0, s.customWidthMm);
        s.customHeightMm = std::max(1.0, s.customHeightMm);
    }
    int orient = s.landscape ? 0 : 1;
    changed |= ImGui::RadioButton("Landscape", &orient, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Portrait", &orient, 1);
    s.landscape = orient == 0;
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Upside down", &s.upsideDown);

    // Area
    static const char* areaNames[] = {"Extents", "Display", "Window"};
    int area = int(s.area);
    if (ImGui::Combo("Plot area", &area, areaNames, 3)) {
        s.area = PlotArea(area);
        changed = true;
    }
    if (s.area == PlotArea::Window) {
        ImGui::TextDisabled(s.window.empty() ? "No window yet: run PLOT, then Area > Window."
                                             : "Window set with the PLOT command (Area > Window).");
    }

    // Scale
    std::string scaleLabel = s.fitToPaper ? "Fit to paper" : scaleText(s);
    if (ImGui::BeginCombo("Scale", scaleLabel.c_str())) {
        if (ImGui::Selectable("Fit to paper", s.fitToPaper)) {
            s.fitToPaper = true;
            changed = true;
        }
        for (double n : exportlib::scalePresets()) {
            std::string label = "1:" + std::to_string(static_cast<long long>(n));
            if (ImGui::Selectable(label.c_str(), !s.fitToPaper && s.scaleN == n)) {
                s.fitToPaper = false;
                s.scaleN = n;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (!s.fitToPaper) {
        changed |= ImGui::InputDouble("1 mm on paper =", &s.scaleN, 1, 10, "%.4g drawing units",
                                      ImGuiInputTextFlags_AutoSelectAll);
        s.scaleN = std::max(1e-6, s.scaleN);
    }

    // Placement
    changed |= ImGui::Checkbox("Centre on paper", &s.centerOnPaper);
    if (!s.centerOnPaper) {
        changed |= ImGui::InputDouble("Offset X (mm)", &s.offsetMm.x, 1, 10, "%.2f",
                                      ImGuiInputTextFlags_AutoSelectAll);
        changed |= ImGui::InputDouble("Offset Y (mm)", &s.offsetMm.y, 1, 10, "%.2f",
                                      ImGuiInputTextFlags_AutoSelectAll);
    }
    changed |= ImGui::InputDouble("Margin (mm)", &s.marginMm, 1, 10, "%.1f",
                                  ImGuiInputTextFlags_AutoSelectAll);
    s.marginMm = std::max(0.0, s.marginMm);

    // Appearance
    changed |= ImGui::Checkbox("Plot lineweights", &s.lineweights);
    changed |= ImGui::Checkbox("Plot transparency", &s.transparency);
    changed |= ImGui::InputDouble("Lineweight scale", &s.lineweightScale, 0.1, 1, "%.2f",
                                  ImGuiInputTextFlags_AutoSelectAll);
    s.lineweightScale = std::clamp(s.lineweightScale, 0.01, 100.0);
    changed |= ImGui::SliderInt("Screening %", &s.screening, 0, 100);
    {
        bool stamp = plotStampOn(*pc.host);
        if (ImGui::Checkbox("Plot stamp", &stamp))
            setPlotStamp(*pc.host, stamp);
        if (stamp) {
            ImGui::SameLine();
            std::string text = plotStampText(*pc.host);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##stamptext", &text))
                setPlotStampText(*pc.host, text);
        }
    }
    {
        bool layers = pdfLayersOn(*pc.host);
        if (ImGui::Checkbox("Include layer information (PDF)", &layers))
            setPdfLayers(*pc.host, layers);
    }
    static const char* colorNames[] = {"As displayed", "Monochrome (black)", "Greyscale"};
    int color = int(s.color);
    if (ImGui::Combo("Colour", &color, colorNames, 3)) {
        s.color = ColorMode(color);
        changed = true;
    }
    if (changed)
        saveSettings(*pc.host, s);

    // Preview, rebuilt only when the document or the settings change.
    BBox2 view = s.area == PlotArea::Display ? viewWindow(*pc.host) : BBox2{};
    PlotSettings built = s;
    if (built.area == PlotArea::Display)
        built.window = view;
    std::string key = keyOf(built, view) + "|" + plotLayout(*pc.host) + "|" + st.styleLoaded +
                      (plotStampOn(*pc.host) ? "|" + plotStampText(*pc.host) : "");
    const bool big = pc.doc->entityCount() > kBigDrawing;
    bool build = key != st.builtKey || pc.doc->revision() != st.builtRevision;
    if (big && st.builtRevision != ~0ull) {
        // A big drawing is not rebuilt after every edit.
        build = st.refreshPreview;
        if (!build) {
            ImGui::TextDisabled("Large drawing: the preview is not updated while you work.");
            if (ImGui::Button("Update preview"))
                st.refreshPreview = true;
        }
    } else if (big) {
        build = st.refreshPreview;
        if (!build) {
            ImGui::TextDisabled("Large drawing: the preview takes a moment to build.");
            if (ImGui::Button("Build preview"))
                st.refreshPreview = true;
        }
    }
    const bool havePreview = build || st.builtRevision != ~0ull;
    if (build) {
        st.refreshPreview = false;
        const std::string name = plotLayout(*pc.host);
        auto sheet = buildPlotSheet(*pc.doc, *pc.host, built);
        st.sheetError = sheet ? std::string() : sheet.error().message;
        st.sheet = sheet ? std::move(sheet.value()) : exportlib::PlotSheet{};
        st.builtKey = key;
        st.builtRevision = pc.doc->revision();
    }
    ImGui::Separator();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float previewH = std::clamp(avail.y - 110.0f, 160.0f, 520.0f);
    if (!havePreview) {
        ImGui::Dummy({avail.x, 4});
    } else if (!st.sheetError.empty()) {
        ImGui::TextDisabled("%s", st.sheetError.c_str());
    } else {
        drawPreview(st.sheet, {avail.x, previewH});
        if (st.sheet.layout.overflows)
            ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f}, "The drawing is larger than the paper.");
        else
            ImGui::TextDisabled("Scale 1:%.4g  |  %zu objects", 1.0 / st.sheet.layout.paperPerUnit,
                                st.sheet.entityCount);
    }

    // Export
    ImGui::InputText("File", &st.fileInput);
    {
        float dpi = static_cast<float>(plotDpi(*pc.host));
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputFloat("Image resolution (dpi)", &dpi, 0, 0, "%.0f"))
            setPlotDpi(*pc.host, std::clamp<double>(dpi, 10, 2400));
    }
    auto doExport = [&](Format f) {
        st.format = f;
        std::string path =
            st.fileInput.empty() ? defaultFile(*pc.host, f) : forFormat(st.fileInput, f);
        {
            std::error_code ec;
            const std::string target = withExtension(path, f);
            if (std::filesystem::exists(platform::pathFromUtf8(target), ec) &&
                st.confirmOverwrite != target) {
                st.confirmOverwrite = target;
                st.message = "\"" + target + "\" already exists. Click Export again to replace it.";
                pc.host->print(st.message);
                return;
            }
            st.confirmOverwrite.clear();
        }
        PlotSettings run = s;
        auto r = exportFile(*pc.doc, *pc.host, run, f, path);
        st.message = r ? r.value() : r.error().message;
        pc.host->print(st.message);
        if (r)
            setLastFile(*pc.host, f, withExtension(path, f));
    };
    if (ImGui::Button("Export PDF"))
        doExport(Format::Pdf);
    ImGui::SameLine();
    if (ImGui::Button("Export SVG"))
        doExport(Format::Svg);
    ImGui::SameLine();
    if (ImGui::Button("Export PNG"))
        doExport(Format::Png);
    ImGui::SameLine();
    if (ImGui::Button("Export JPEG"))
        doExport(Format::Jpeg);

    // Printer
    ImGui::Separator();
    if (!print::available()) {
        ImGui::TextDisabled("Printing is available in the Windows version. Export a PDF instead.");
    } else {
        const std::string chosen = pc.host->setting("plot.printer").value_or("");
        if (ImGui::BeginCombo("Printer", chosen.empty() ? "<default printer>" : chosen.c_str())) {
            if (ImGui::Selectable("<default printer>", chosen.empty()))
                pc.host->setSetting("plot.printer", "");
            for (const auto& p : print::listPrinters())
                if (ImGui::Selectable(p.name.c_str(), p.name == chosen))
                    pc.host->setSetting("plot.printer", p.name);
            ImGui::EndCombo();
        }
        int copies =
            std::clamp(std::atoi(pc.host->setting("plot.copies").value_or("1").c_str()), 1, 99);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Copies", &copies)) {
            copies = std::clamp(copies, 1, 99);
            pc.host->setSetting("plot.copies", std::to_string(copies));
        }
        auto doPrint = [&](bool dialog) {
            auto sheet = buildPlotSheet(*pc.doc, *pc.host, s);
            if (!sheet) {
                st.message = sheet.error().message;
                return;
            }
            print::PrintJob job = printJob(*pc.host, dialog);
            std::string picked;
            job.chosenPrinter = &picked;
            auto r = print::printSheet(sheet.value(), job);
            if (!picked.empty())
                pc.host->setSetting("plot.printer", picked);
            st.message =
                r ? "Sent " + std::to_string(sheet.value().entityCount) + " objects to the printer."
                  : r.error().message;
            pc.host->print(st.message);
        };
        if (ImGui::Button("Print"))
            doPrint(false);
        ImGui::SameLine();
        if (ImGui::Button("Print dialog..."))
            doPrint(true);
    }
    if (!st.message.empty())
        ImGui::TextWrapped("%s", st.message.c_str());
}

// Runs the lines one after another, as typed: a command and the answers to its prompts (names
// with spaces stay whole this way).
void runLines(ui::PanelContext& pc, std::initializer_list<std::string> lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

// PAGESETUPPANEL: the page setups of the drawing, which sheet uses which, and the buttons to make
// one current, create, change, rename, delete and import them.
struct SetupState {
    std::string selected{};
    std::string nameInput{};
    std::string importInput{};
    enum class Ask { None, New, Rename, Import } ask = Ask::None;
};

void drawPageSetups(ui::PanelContext& pc) {
    static SetupState st;
    if (!pc.doc || !pc.host)
        return;
    const std::string sheet = plotLayout(*pc.host);
    const std::string where = sheet.empty() ? std::string("Model") : sheet;
    std::string inUse = layout::modelPageSetup(*pc.doc);
    if (!sheet.empty())
        if (auto l = layout::findLayout(*pc.doc, sheet))
            inUse = l->pageSetup;
    ImGui::Text("Current: %s", where.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("page setup: %s", inUse.empty() ? "<none>" : inUse.c_str());

    const auto all = layout::pageSetups(*pc.doc);
    if (st.selected.empty() || !layout::findPageSetup(*pc.doc, st.selected))
        st.selected = !inUse.empty() ? inUse : all.empty() ? std::string() : all.front().name;
    const float listH = std::max(120.0f, ImGui::GetContentRegionAvail().y - 150.0f);
    if (ImGui::BeginTable("##setups", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Paper");
        ImGui::TableSetupColumn("Scale");
        ImGui::TableSetupColumn("Plot style");
        ImGui::TableSetupColumn("Used by");
        ImGui::TableHeadersRow();
        const std::string model = layout::modelPageSetup(*pc.doc);
        const auto sheets = layout::layouts(*pc.doc);
        for (const auto& ps : all) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = (ps.name == inUse ? "* " : "  ") + ps.name;
            if (ImGui::Selectable(
                    label.c_str(), st.selected == ps.name,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                st.selected = ps.name;
                if (ImGui::IsMouseDoubleClicked(0))  // double click: make it current
                    runLines(
                        pc, {"PAGESETUP", "Assign", ps.name, where, "PAGESETUP", "Apply", ps.name});
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                (ps.settings.paperName + (ps.settings.landscape ? " landscape" : " portrait"))
                    .c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(scaleText(ps.settings).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ps.styleName.empty() ? "none" : ps.styleName.c_str());
            ImGui::TableNextColumn();
            std::string users = ps.name == model ? "Model" : "";
            for (const auto& l : sheets)
                if (l.pageSetup == ps.name)
                    users += (users.empty() ? "" : ", ") + l.name;
            ImGui::TextUnformatted(users.c_str());
        }
        ImGui::EndTable();
    }
    if (all.empty())
        ImGui::TextDisabled("No page setups yet. New saves the current plot options under a name.");

    const bool have = !st.selected.empty();
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Set current"))
        runLines(pc,
                 {"PAGESETUP", "Assign", st.selected, where, "PAGESETUP", "Apply", st.selected});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Use this page setup for %s", where.c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New...")) {
        st.ask = SetupState::Ask::New;
        st.nameInput = "Setup" + std::to_string(all.size() + 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Modify...")) {
        // The plot options take this setup's values; the Plot panel edits them and saves them
        // back under the same name.
        runLines(pc, {"PAGESETUP", "Apply", st.selected});
        pc.host->setSetting("tmp.plot.setupName", st.selected);
        pc.host->openPanel("plot");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename...")) {
        st.ask = SetupState::Ask::Rename;
        st.nameInput = st.selected;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        runLines(pc, {"PAGESETUP", "Delete", st.selected});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Import..."))
        st.ask = SetupState::Ask::Import;

    if (st.ask == SetupState::Ask::New || st.ask == SetupState::Ask::Rename) {
        ImGui::SetNextItemWidth(220);
        const bool enter =
            ImGui::InputText(st.ask == SetupState::Ask::New ? "New name" : "Rename to",
                             &st.nameInput, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("OK") || enter) && !st.nameInput.empty()) {
            if (st.ask == SetupState::Ask::New)
                runLines(pc, {"PAGESETUP", "Save", st.nameInput});
            else
                runLines(pc, {"PAGESETUP", "Rename", st.selected, st.nameInput});
            st.selected = st.nameInput;
            st.ask = SetupState::Ask::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            st.ask = SetupState::Ask::None;
    } else if (st.ask == SetupState::Ask::Import) {
        ImGui::SetNextItemWidth(-140);
        const bool enter =
            ImGui::InputTextWithHint("##importfile", "Drawing (.tbcad or .dxf)", &st.importInput,
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Import") || enter) && !st.importInput.empty()) {
            std::string f = st.importInput;
            if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                f = f.substr(1, f.size() - 2);
            runLines(pc, {"PAGESETUP", "Import", f});
            st.ask = SetupState::Ask::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            st.ask = SetupState::Ask::None;
    }
    if (const auto p = layout::findPageSetup(*pc.doc, st.selected)) {
        const auto& ss = p->settings;
        ImGui::SeparatorText("Details");
        ImGui::Text("%s, %s, scale %s, margins %.4g mm", ss.paperName.c_str(),
                    ss.landscape ? "landscape" : "portrait", scaleText(ss).c_str(), ss.marginMm);
        ImGui::Text("Plot style %s, %s", p->styleName.empty() ? "none" : p->styleName.c_str(),
                    ss.lineweights ? "with lineweights" : "no lineweights");
    }
}

// PREVIEW: the sheet as it will plot, as large as the panel allows.
void drawPlotPreview(ui::PanelContext& pc) {
    static exportlib::PlotSheet sheet;
    static std::string error;
    static std::string builtKey;
    static std::uint64_t builtRevision = ~0ull;
    static bool refresh = false;
    if (!pc.doc || !pc.host)
        return;
    PlotSettings s = loadSettings(*pc.host);
    const BBox2 view = s.area == PlotArea::Display ? viewWindow(*pc.host) : BBox2{};
    if (s.area == PlotArea::Display)
        s.window = view;
    const std::string key =
        keyOf(s, view) + "|" + plotLayout(*pc.host) + "|" + plotStyleName(*pc.host);
    bool build = key != builtKey || pc.doc->revision() != builtRevision;
    // A big drawing takes seconds to build: only on request, as in the Plot panel.
    if (build && pc.doc->entityCount() > kBigDrawing && !refresh) {
        ImGui::TextDisabled(builtRevision == ~0ull
                                ? "Large drawing: the preview takes a moment to build."
                                : "Large drawing: the preview is not updated while you work.");
        ImGui::SameLine();
        if (ImGui::Button(builtRevision == ~0ull ? "Build preview" : "Update preview"))
            refresh = true;
        build = false;
        if (builtRevision == ~0ull)
            return;
    }
    if (build) {
        refresh = false;
        auto built = buildPlotSheet(*pc.doc, *pc.host, s);
        error = built ? std::string() : built.error().message;
        sheet = built ? std::move(built.value()) : exportlib::PlotSheet{};
        builtKey = key;
        builtRevision = pc.doc->revision();
    }
    const std::string where = plotLayout(*pc.host).empty() ? "Model" : plotLayout(*pc.host);
    ImGui::Text("%s: %s %s, %s", where.c_str(), s.paperName.c_str(),
                s.landscape ? "landscape" : "portrait", scaleText(s).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Plot..."))
        pc.host->openPanel("plot");
    if (!error.empty()) {
        ImGui::TextDisabled("%s", error.c_str());
        return;
    }
    if (sheet.layout.overflows)
        ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f}, "The drawing is larger than the paper.");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    drawPreview(sheet, {avail.x, std::max(120.0f, avail.y)});
}

}  // namespace

void registerPlotPanel(ui::UiRegistry& registry) {
    registry.addPanel("plot", "Plot", draw, false);
    registry.addPanel("pagesetup", "Page Setup Manager", drawPageSetups, false);
    registry.addPanel("plotpreview", "Plot Preview", drawPlotPreview, false);
    registerStylePanel(registry);
    registerPublishPanel(registry);
}

}  // namespace tbcad::plot
