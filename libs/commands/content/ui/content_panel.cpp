#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../src/commands.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/palettes/palettes.h"
#include "tbcad/ui/drag_payload.h"
#include "tbcad/xref/xref.h"

namespace tbcad::content_cmds {

namespace {

namespace fs = std::filesystem;

constexpr const char* kFolderSetting = "content.folder";

struct State {
    std::string folder;
    std::string folderEdit;
    fs::path file;  // the drawing being browsed
    std::shared_ptr<xref::Source> source;
    std::vector<xref::ContentItem> items;
    std::string error;
    std::set<std::string> selected;  // "<kind>:<name>"
    int tab = 0;
};

State& state() {
    static State s;
    return s;
}

std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathOf(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return e;
}

bool isDrawing(const fs::path& p) {
    const std::string e = lowerExt(p);
    return e == ".tbcad" || e == ".dxf" || e == ".tbt";
}

void openFile(State& st, const fs::path& file) {
    st.file = file;
    st.selected.clear();
    st.items.clear();
    st.error.clear();
    st.source.reset();
    // A template is an ordinary .tbcad file; loadSource keys on the extension.
    auto res = xref::loadSource(file);
    if (!res) {
        st.error = res.error().message;
        return;
    }
    st.source = std::make_shared<xref::Source>(std::move(res.value()));
    st.items = xref::listContent(*st.source->doc);
}

const char* kindName(xref::ContentKind k) {
    switch (k) {
        case xref::ContentKind::Block:
            return "Block";
        case xref::ContentKind::Layer:
            return "Layer";
        case xref::ContentKind::Linetype:
            return "Linetype";
        case xref::ContentKind::TextStyle:
            return "Textstyle";
        case xref::ContentKind::DimStyle:
            return "Dimstyle";
    }
    return "Block";
}

std::string keyOf(const xref::ContentItem& i) {
    return std::string(kindName(i.kind)) + ":" + i.name;
}

void drawThumb(ImDrawList* dl, ImVec2 topLeft, float size, const block_cmds::Thumbnail& t,
               ImU32 color) {
    if (t.empty())
        return;
    const float ox = topLeft.x + (size - static_cast<float>(t.width) * size) / 2;
    const float oy = topLeft.y + (size + static_cast<float>(t.height) * size) / 2;
    std::vector<ImVec2> pts;
    for (const auto& stroke : t.strokes) {
        pts.clear();
        for (Vec2 p : stroke)
            pts.emplace_back(ox + static_cast<float>(p.x) * size,
                             oy - static_cast<float>(p.y) * size);
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, ImDrawFlags_None, 1.0f);
    }
}

// Runs CONTENTIMPORT for the selected items of one kind.
void importSelected(ui::PanelContext& pc, State& st, xref::ContentKind kind) {
    std::string names;
    for (const auto& i : st.items)
        if (i.kind == kind && st.selected.count(keyOf(i)))
            names += (names.empty() ? "" : "|") + i.name;
    if (names.empty() || !pc.runCommand)
        return;
    pc.runCommand("CONTENTIMPORT");
    pc.runCommand(utf8(st.file));
    pc.runCommand(kindName(kind));
    pc.runCommand(names);
}

void drawFiles(ui::PanelContext& pc, State& st) {
    if (st.folder.empty()) {
        std::string start = pc.host ? pc.host->setting(kFolderSetting).value_or("") : "";
        if (start.empty() && pc.host)
            start = pc.host->documentFile();
        fs::path p = pathOf(start);
        if (!start.empty() && !fs::is_directory(p))
            p = p.parent_path();
        if (p.empty() || !fs::is_directory(p))
            p = fs::current_path();
        st.folder = utf8(p);
        st.folderEdit = st.folder;
    }
    if (ImGui::Button("Up")) {
        st.folder = utf8(pathOf(st.folder).parent_path());
        st.folderEdit = st.folder;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##contentFolder", &st.folderEdit, ImGuiInputTextFlags_EnterReturnsTrue) &&
        fs::is_directory(pathOf(st.folderEdit))) {
        st.folder = st.folderEdit;
        if (pc.host)
            pc.host->setSetting(kFolderSetting, st.folder);
    }
    if (ImGui::IsItemHovered() && !st.folderEdit.empty())
        ImGui::SetTooltip("%s", st.folderEdit.c_str());  // the box is narrow in a docked panel
    std::vector<fs::path> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(pathOf(st.folder), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec))
            dirs.push_back(it->path());
        else if (it->is_regular_file(ec) && isDrawing(it->path()))
            files.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    ImGui::BeginChild("##contentFiles", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const fs::path& d : dirs) {
        const std::string label = "[" + utf8(d.filename()) + "]";
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(0)) {
            st.folder = utf8(d);
            st.folderEdit = st.folder;
            if (pc.host)
                pc.host->setSetting(kFolderSetting, st.folder);
            break;
        }
    }
    for (const fs::path& f : files)
        if (ImGui::Selectable(utf8(f.filename()).c_str(), f == st.file))
            openFile(st, f);
    ImGui::EndChild();
}

// Lists the palettes; choosing one adds the selected blocks to it as tools that copy the block
// from this file when the drawing does not have it yet.
void addToPaletteMenu(ui::PanelContext& pc, State& st) {
    const std::filesystem::path folder = pc.host ? pc.host->settingsFolder() : fs::path{};
    if (folder.empty()) {
        ImGui::TextDisabled("No settings folder.");
        return;
    }
    palettes::PaletteStore store(folder / "palettes.json");
    store.load();
    for (const palettes::Palette& p : store.palettes()) {
        if (!ImGui::MenuItem(p.name.c_str()))
            continue;
        for (const auto& i : st.items) {
            if (i.kind != xref::ContentKind::Block || !st.selected.count(keyOf(i)))
                continue;
            palettes::Tool t;
            t.name = i.name;
            t.kind = palettes::ToolKind::Block;
            t.block = i.name;
            t.file = utf8(st.file);
            (void)store.addTool(p.name, std::move(t));  // a name already on the palette is skipped
        }
        st.error = store.save();
    }
}

void drawItems(ui::PanelContext& pc, State& st) {
    if (!st.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "%s", st.error.c_str());
    if (!st.source) {
        ImGui::TextDisabled("Pick a drawing on the left.");
        return;
    }
    ImGui::TextDisabled("%s", utf8(st.file.filename()).c_str());
    static const xref::ContentKind kinds[] = {
        xref::ContentKind::Block, xref::ContentKind::Layer, xref::ContentKind::Linetype,
        xref::ContentKind::TextStyle, xref::ContentKind::DimStyle};
    static const char* titles[] = {"Blocks", "Layers", "Linetypes", "Text styles", "Dim styles"};
    if (!ImGui::BeginTabBar("##contentKinds"))
        return;
    for (int k = 0; k < 5; ++k) {
        if (!ImGui::BeginTabItem(titles[k]))
            continue;
        const xref::ContentKind kind = kinds[k];
        const float footer = ImGui::GetFrameHeightWithSpacing() + 4;
        ImGui::BeginChild("##contentItems", ImVec2(0, -footer), ImGuiChildFlags_Borders);
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        for (const auto& item : st.items) {
            if (item.kind != kind)
                continue;
            const std::string key = keyOf(item);
            const float row = kind == xref::ContentKind::Block ? 36.0f : 0.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            bool on = st.selected.count(key) != 0;
            if (ImGui::Selectable(("##" + key).c_str(), on, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, row))) {
                if (!ImGui::GetIO().KeyCtrl)
                    st.selected.clear();
                if (on && ImGui::GetIO().KeyCtrl)
                    st.selected.erase(key);
                else
                    st.selected.insert(key);
            }
            // Drag onto the drawing (copy, and insert a block) or onto a palette (add as tool).
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                if (!st.selected.count(key)) {
                    st.selected.clear();
                    st.selected.insert(key);
                }
                ui::ContentDrag drag;
                drag.file = utf8(st.file);
                drag.kind = kindName(kind);
                for (const auto& i : st.items)
                    if (i.kind == kind && st.selected.count(keyOf(i)))
                        drag.names.push_back(i.name);
                const std::string data = ui::encodeContent(drag);
                ImGui::SetDragDropPayload(ui::kContentPayload, data.c_str(), data.size() + 1);
                ImGui::Text("%s: %s%s", drag.kind.c_str(), item.name.c_str(),
                            drag.names.size() > 1 ? " ..." : "");
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
            if (kind == xref::ContentKind::Block) {
                if (const BlockDef* b = st.source->doc->blockByName(item.name))
                    drawThumb(ImGui::GetWindowDrawList(), ImVec2(at.x + 4, at.y + 2), 32.0f,
                              block_cmds::thumbnailOfBlock(*st.source->doc, b->id), ink);
                ImGui::SetCursorScreenPos(ImVec2(at.x + 44, at.y + 9));
            }
            ImGui::TextUnformatted(item.name.c_str());
            if (kind == xref::ContentKind::Block)
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + row + 2));
        }
        ImGui::EndChild();
        if (ImGui::Button("Select all")) {
            for (const auto& i : st.items)
                if (i.kind == kind)
                    st.selected.insert(keyOf(i));
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy to drawing"))
            importSelected(pc, st, kind);
        if (kind == xref::ContentKind::Block) {
            ImGui::SameLine();
            ImGui::SameLine();
            if (ImGui::Button("Add to palette"))
                ImGui::OpenPopup("##addToPalette");
            if (ImGui::BeginPopup("##addToPalette")) {
                addToPaletteMenu(pc, st);
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy and insert")) {
                importSelected(pc, st, kind);
                for (const auto& i : st.items)
                    if (i.kind == kind && st.selected.count(keyOf(i)) && pc.runCommand) {
                        pc.runCommand("INSERT " + i.name);
                        break;
                    }
            }
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void drawPanel(ui::PanelContext& pc) {
    State& st = state();
    if (!pc.doc)
        return;
    ImGui::BeginChild("##contentLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0));
    drawFiles(pc, st);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##contentRight", ImVec2(0, 0));
    drawItems(pc, st);
    ImGui::EndChild();
}

}  // namespace

void registerContentPanel(ui::UiRegistry& ui) {
    ui.addPanel("content", "Content Browser", drawPanel, false);
}

}  // namespace tbcad::content_cmds
