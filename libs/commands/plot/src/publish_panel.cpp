#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "panel.h"
#include "tbcad/layout/pagesetup.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"
#include "tbcad/sheets/sheets.h"

namespace tbcad::plot {

namespace {

// The PUBLISH dialog: a sheet list made of the model and layouts of several drawings, a page
// setup per sheet, saved and loaded as a list file (.tbdsd, the format PLOTSET reads), published
// to one multi-page PDF through -PUBLISH so it shows on the command line like a typed plot.
struct PublishState {
    std::vector<sheets::SheetEntry> entries{};
    int selected = -1;
    std::string addPath{};
    std::string listPath{};
    std::string outPath{};
    std::string message{};
    bool seeded = false;
};

PublishState& publishState() {
    static PublishState s;
    return s;
}

std::string fileName(const std::filesystem::path& p) {
    return platform::pathToUtf8(p.filename());
}

std::string withoutExtension(const std::string& path) {
    auto p = platform::pathFromUtf8(path);
    p.replace_extension();
    return platform::pathToUtf8(p);
}

void addSheets(PublishState& st, const std::vector<sheets::SheetEntry>& more) {
    std::size_t added = 0;
    for (const auto& e : more) {
        if (std::find(st.entries.begin(), st.entries.end(), e) != st.entries.end())
            continue;
        st.entries.push_back(e);
        ++added;
    }
    st.message = "Added " + std::to_string(added) + (added == 1 ? " sheet." : " sheets.");
}

// The list the Publish button writes for -PUBLISH: in the settings folder, or next to the output.
std::filesystem::path scratchList(const cmd::IHost& host, const std::string& out) {
    auto folder = host.settingsFolder();
    if (folder.empty())
        return platform::pathFromUtf8(withoutExtension(out) + ".tbdsd");
    platform::ensureDirectory(folder);
    return folder / "publish-last.tbdsd";
}

void drawPublish(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    PublishState& st = publishState();
    const std::string current = pc.host->documentFile();
    if (!st.seeded) {
        st.seeded = true;
        if (!current.empty()) {
            st.entries = sheets::sheetsOf(*pc.doc, platform::pathFromUtf8(current));
            st.outPath = withoutExtension(current) + ".pdf";
            st.listPath = withoutExtension(current) + ".tbdsd";
        }
    }

    if (ImGui::Button("Add current drawing's sheets")) {
        if (current.empty())
            st.message = "Save the drawing first: a sheet list names drawing files.";
        else
            addSheets(st, sheets::sheetsOf(*pc.doc, platform::pathFromUtf8(current)));
    }
    ImGui::SetNextItemWidth(-140);
    ImGui::InputTextWithHint("##adddrawing", "Drawing to add (.tbcad or .dxf)", &st.addPath);
    ImGui::SameLine();
    if (ImGui::Button("Add drawing")) {
        const auto file = platform::pathFromUtf8(st.addPath);
        auto other = readDrawingFile(file);
        if (!other)
            st.message = other.error().message;
        else
            addSheets(st, sheets::sheetsOf(*other.value(), file));
    }

    const auto setups = layout::pageSetups(*pc.doc);
    const float listH = std::max(120.0f, ImGui::GetContentRegionAvail().y - 170.0f);
    if (ImGui::BeginTable("##sheetlist", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Drawing");
        ImGui::TableSetupColumn("Layout");
        ImGui::TableSetupColumn("Page setup");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(st.entries.size()); ++i) {
            auto& e = st.entries[std::size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(
                    std::to_string(i + 1).c_str(), st.selected == i,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                st.selected = i;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fileName(e.file).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", platform::pathToUtf8(e.file).c_str());
            ImGui::TableNextColumn();
            const std::string sheetName = !e.layout.empty()  ? e.layout
                                          : !e.frame.empty() ? "Model: " + e.frame
                                                             : std::string("Model");
            ImGui::TextUnformatted(sheetName.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            const char* shown =
                e.pageSetup.empty() ? "<Default: layout's own>" : e.pageSetup.c_str();
            if (ImGui::BeginCombo("##setup", shown)) {
                if (ImGui::Selectable("<Default: layout's own>", e.pageSetup.empty()))
                    e.pageSetup.clear();
                for (const auto& s : setups)
                    if (ImGui::Selectable(s.name.c_str(), e.pageSetup == s.name))
                        e.pageSetup = s.name;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool have = st.selected >= 0 && st.selected < int(st.entries.size());
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Remove")) {
        st.entries.erase(st.entries.begin() + st.selected);
        st.selected = std::min(st.selected, int(st.entries.size()) - 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!have || st.selected == 0);
    if (ImGui::Button("Move up")) {
        std::swap(st.entries[std::size_t(st.selected)], st.entries[std::size_t(st.selected - 1)]);
        --st.selected;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!have || st.selected + 1 >= int(st.entries.size()));
    if (ImGui::Button("Move down")) {
        std::swap(st.entries[std::size_t(st.selected)], st.entries[std::size_t(st.selected + 1)]);
        ++st.selected;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(st.entries.empty());
    if (ImGui::Button("Clear")) {
        st.entries.clear();
        st.selected = -1;
    }
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(-200);
    ImGui::InputTextWithHint("##listfile", "Sheet list file (.tbdsd)", &st.listPath);
    ImGui::SameLine();
    if (ImGui::Button("Save list")) {
        const std::string err =
            sheets::writeSheetList(st.entries, platform::pathFromUtf8(st.listPath));
        st.message = err.empty() ? "Saved " + st.listPath : err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load list")) {
        auto read = sheets::readSheetList(platform::pathFromUtf8(st.listPath));
        if (!read) {
            st.message = read.error().message;
        } else {
            st.entries = std::move(read.value());
            st.selected = -1;
            st.message = "Loaded " + std::to_string(st.entries.size()) + " sheets.";
        }
    }

    ImGui::SetNextItemWidth(-200);
    ImGui::InputTextWithHint("##pdfout", "Output PDF", &st.outPath);
    {
        ImGui::SameLine();
        bool layers = pdfLayersOn(*pc.host);
        if (ImGui::Checkbox("Layer information", &layers))
            setPdfLayers(*pc.host, layers);
        bool stamp = plotStampOn(*pc.host);
        if (ImGui::Checkbox("Plot stamp", &stamp))
            setPlotStamp(*pc.host, stamp);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(st.entries.empty() || st.outPath.empty() || !pc.runCommand);
    if (ImGui::Button("Publish to PDF")) {
        const auto list = scratchList(*pc.host, st.outPath);
        const std::string err = sheets::writeSheetList(st.entries, list);
        if (!err.empty()) {
            st.message = err;
        } else {
            if (pc.cancelCommand)
                pc.cancelCommand();
            pc.runCommand("-PUBLISH");
            pc.runCommand(platform::pathToUtf8(list));
            pc.runCommand(st.outPath);
            st.message = "Published to " + withExtension(st.outPath, Format::Pdf) +
                         " (see the command line).";
        }
    }
    ImGui::EndDisabled();
    if (!st.message.empty())
        ImGui::TextWrapped("%s", st.message.c_str());
}

}  // namespace

void registerPublishPanel(ui::UiRegistry& registry) {
    registry.addPanel("publish", "Publish", drawPublish, false);
}

}  // namespace tbcad::plot
