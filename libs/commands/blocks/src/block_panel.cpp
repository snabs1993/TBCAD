#include <array>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "commands.h"
#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/block_cmds/block_list.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/ui/drag_payload.h"
#include "tbcad/ui/recent_paths.h"
#include "tbcad/xref/xref.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

constexpr float kTile = 88.0f;  // width of one palette tile
constexpr float kThumb = 64.0f;
constexpr const char* kLibrarySetting = "block.library";

// Draws the strokes of a thumbnail inside a square of `size` pixels at `topLeft`.
void drawThumbnail(ImDrawList* dl, ImVec2 topLeft, float size, const Thumbnail& t, ImU32 color) {
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

std::string utf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct TileResult {
    bool clicked = false;
    bool doubleClicked = false;
    bool hovered = false;
};

// One palette tile: thumbnail, name, small caption. The caller adds popups after it.
TileResult drawTile(const char* id, const std::string& name, const std::string& caption,
                    const Thumbnail* thumb, bool selected) {
    TileResult r;
    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = kThumb + 2 * ImGui::GetTextLineHeightWithSpacing() + 6;
    ImGui::InvisibleButton("##tile", ImVec2(kTile, height));
    r.hovered = ImGui::IsItemHovered();
    r.clicked = ImGui::IsItemClicked(0);
    r.doubleClicked = r.hovered && ImGui::IsMouseDoubleClicked(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || r.hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + kTile, pos.y + height),
                          ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered),
                          4.0f);
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    if (thumb)
        drawThumbnail(dl, ImVec2(pos.x + (kTile - kThumb) / 2, pos.y + 3), kThumb, *thumb, ink);
    else
        dl->AddText(ImVec2(pos.x + 8, pos.y + 24), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    "loading");
    dl->PushClipRect(ImVec2(pos.x + 2, pos.y), ImVec2(pos.x + kTile - 2, pos.y + height), true);
    dl->AddText(ImVec2(pos.x + 4, pos.y + kThumb + 6), ink, name.c_str());
    dl->AddText(ImVec2(pos.x + 4, pos.y + kThumb + 6 + ImGui::GetTextLineHeightWithSpacing()),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), caption.c_str());
    dl->PopClipRect();
    if (r.hovered)
        ImGui::SetTooltip("%s", name.c_str());
    ImGui::PopID();
    return r;
}

// ---- Options: how the palette places a block --------------------------------------------------

struct PaletteOptions {
    InsertOptions insert{};
    bool askScale = false;     // scale on screen, after the point
    bool askRotation = false;  // rotation on screen, after the point
    bool loaded = false;
};

double numberSetting(cmd::IHost* host, const char* key, double fallback) {
    if (!host)
        return fallback;
    auto v = host->setting(key);
    if (!v || v->empty())
        return fallback;
    char* end = nullptr;
    const double d = std::strtod(v->c_str(), &end);
    return end != v->c_str() ? d : fallback;
}

bool flagSetting(cmd::IHost* host, const char* key) {
    return host && host->setting(key).value_or("0") == "1";
}

PaletteOptions& options(cmd::IHost* host) {
    static PaletteOptions o;
    if (!o.loaded && host) {
        o.insert.scale = numberSetting(host, "block.palette.scale", 1);
        if (o.insert.scale == 0)
            o.insert.scale = 1;
        o.insert.rotationDeg = numberSetting(host, "block.palette.rotation", 0);
        o.insert.repeat = flagSetting(host, "block.palette.repeat");
        o.insert.explode = flagSetting(host, "block.palette.explode");
        o.askScale = flagSetting(host, "block.palette.askScale");
        o.askRotation = flagSetting(host, "block.palette.askRotation");
        o.loaded = true;
    }
    return o;
}

void saveOptions(cmd::IHost* host, const PaletteOptions& o) {
    if (!host)
        return;
    host->setSetting("block.palette.scale", formatDouble(o.insert.scale));
    host->setSetting("block.palette.rotation", formatDouble(o.insert.rotationDeg));
    host->setSetting("block.palette.repeat", o.insert.repeat ? "1" : "0");
    host->setSetting("block.palette.explode", o.insert.explode ? "1" : "0");
    host->setSetting("block.palette.askScale", o.askScale ? "1" : "0");
    host->setSetting("block.palette.askRotation", o.askRotation ? "1" : "0");
}

void drawOptions(ui::PanelContext& ctx) {
    PaletteOptions& o = options(ctx.host);
    if (!ImGui::CollapsingHeader("Options"))
        return;
    bool changed = false;
    ImGui::BeginDisabled(o.askScale);
    ImGui::SetNextItemWidth(90);
    changed |= ImGui::InputDouble("Scale", &o.insert.scale, 0, 0, "%.4g");
    ImGui::EndDisabled();
    ImGui::SameLine();
    changed |= ImGui::Checkbox("On screen##scale", &o.askScale);
    ImGui::BeginDisabled(o.askRotation);
    ImGui::SetNextItemWidth(90);
    changed |= ImGui::InputDouble("Rotation", &o.insert.rotationDeg, 0, 0, "%.4g");
    ImGui::EndDisabled();
    ImGui::SameLine();
    changed |= ImGui::Checkbox("On screen##rotation", &o.askRotation);
    changed |= ImGui::Checkbox("Repeat placement", &o.insert.repeat);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep placing copies until Enter or Esc");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Explode", &o.insert.explode);
    if (o.insert.scale == 0)
        o.insert.scale = 1;
    if (changed)
        saveOptions(ctx.host, o);
}

// Places a block with the palette's options. `file` is the drawing the block comes from (copied
// in first when this drawing does not have it); `whole` inserts that whole file as a block.
void placeBlock(ui::PanelContext& ctx, const std::string& name, const std::string& file = {},
                bool whole = false) {
    if (!ctx.runCommand || !ctx.host || !ctx.doc)
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    const PaletteOptions& o = options(ctx.host);
    InsertOptions io = o.insert;
    if (o.askScale)
        io.scale = 1;
    if (o.askRotation)
        io.rotationDeg = 0;
    ctx.host->setSetting("tmp.insert.ask",
                         std::string(o.askScale ? "S" : "") + (o.askRotation ? "R" : ""));
    if (whole) {
        for (const std::string& line : paletteInsertLines(file, io))
            ctx.runCommand(line);
        return;
    }
    if (!file.empty()) {
        ctx.host->setSetting("tmp.insert.source", file);
        if (!ctx.doc->blockByName(name)) {
            ctx.runCommand("CONTENTIMPORT");
            ctx.runCommand(file);
            ctx.runCommand("Block");
            ctx.runCommand(name);
        }
    }
    for (const std::string& line : paletteInsertLines(name, io))
        ctx.runCommand(line);
}

// A tile dragged onto the drawing (inserted where it is dropped) or onto a tool palette.
void dragSource(const std::string& file, const std::string& name, const Thumbnail* thumb) {
    if (!ImGui::BeginDragDropSource())
        return;
    const std::string data = ui::encodeContent({file, "Block", {name}});
    ImGui::SetDragDropPayload(ui::kContentPayload, data.c_str(), data.size() + 1);
    if (thumb)
        drawThumbnail(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 48, *thumb,
                      ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::Dummy(ImVec2(48, 48));
    ImGui::TextUnformatted(name.c_str());
    ImGui::EndDragDropSource();
}

// Lays out tiles in rows: call after each tile.
void nextTile(float& used) {
    used += kTile + ImGui::GetStyle().ItemSpacing.x;
    if (used + kTile > ImGui::GetContentRegionAvail().x)
        used = 0;
    else
        ImGui::SameLine();
}

// ---- Current Drawing tab -----------------------------------------------------------------------

struct DrawingTab {
    const Document* doc = nullptr;
    std::map<BlockId, std::pair<std::uint64_t, Thumbnail>> cache{};
    BlockId selected{};
};

void drawDrawingTab(ui::PanelContext& ctx) {
    static DrawingTab st;
    static std::string filter;
    if (st.doc != ctx.doc) {
        st = DrawingTab{};
        st.doc = ctx.doc;
    }
    ImGui::SetNextItemWidth(-FLT_MIN - 100);
    ImGui::InputTextWithHint("##blockfilter", "Filter blocks", &filter);
    ImGui::SameLine();
    if (ImGui::Button("Count uses") && ctx.runCommand)
        ctx.runCommand("BLOCKCOUNT");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Count the inserts of each block (BLOCKCOUNT)");
    const auto rows = blockRows(*ctx.doc, filter);
    if (rows.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(filter.empty() ? "No blocks yet. Select objects and type B."
                                           : "No block matches the filter");
        return;
    }
    ImGui::BeginChild("##blocktiles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    int made = 0;  // thumbnails built this frame
    float used = 0;
    for (const BlockRow& r : rows) {
        auto& slot = st.cache[r.id];
        const std::uint64_t rev = ctx.doc->revision();
        if ((slot.first != rev + 1) && made < 4) {
            slot = {rev + 1, thumbnailOfBlock(*ctx.doc, r.id)};
            ++made;
        }
        // Read once: the menu items below run commands that can rewrite the block table.
        const BlockDef* def = ctx.doc->block(r.id);
        const bool isXref = def && !def->xrefPath.empty();
        std::string caption = isXref ? "xref" : "x" + std::to_string(r.references);
        const Thumbnail* thumb = slot.first ? &slot.second : nullptr;
        TileResult t = drawTile(r.name.c_str(), r.name, caption, thumb, st.selected == r.id);
        if (!isXref)
            dragSource({}, r.name, thumb);
        if (t.clicked)
            st.selected = r.id;
        if (t.doubleClicked)
            placeBlock(ctx, r.name);
        if (ImGui::BeginPopupContextItem("##tilemenu")) {
            st.selected = r.id;
            if (ImGui::MenuItem("Insert"))
                placeBlock(ctx, r.name);
            if (!isXref && ImGui::MenuItem("Edit block") && ctx.runCommand) {
                ctx.runCommand("BEDIT");
                ctx.runCommand("Name");
                ctx.runCommand(r.name);
            }
            if (isXref && ctx.runCommand) {
                if (ImGui::MenuItem("Reload xref")) {
                    ctx.runCommand("XRELOAD");
                    ctx.runCommand(r.name);
                }
                if (ImGui::MenuItem("Unload xref")) {
                    ctx.runCommand("XUNLOAD");
                    ctx.runCommand(r.name);
                }
                if (ImGui::MenuItem("Bind xref")) {
                    ctx.runCommand("XBIND");
                    ctx.runCommand(r.name);
                    ctx.runCommand("Bind");
                }
                if (ImGui::MenuItem("Bind xref (merge layers)")) {
                    ctx.runCommand("XBIND");
                    ctx.runCommand(r.name);
                    ctx.runCommand("Insert");
                }
                if (ImGui::BeginMenu("Path type")) {
                    for (const char* mode : {"Relative", "Absolute", "None"})
                        if (ImGui::MenuItem(mode)) {
                            ctx.runCommand("XPATH");
                            ctx.runCommand(r.name);
                            ctx.runCommand(mode);
                        }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Detach xref")) {
                    ctx.runCommand("XDETACH");
                    ctx.runCommand(r.name);
                }
            }
            ImGui::EndPopup();
        }
        nextTile(used);
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click or drag a block to insert it; right-click for more");
}

// ---- Other Drawing tab and the drawings it reads -----------------------------------------------

// Drawings read for the Recent and Other Drawing tabs, with block thumbnails. Read again when the
// file changes on disk.
struct SourceCache {
    struct Entry {
        std::int64_t stamp = -1;
        std::shared_ptr<xref::Source> source{};
        std::string error{};
        std::map<std::string, Thumbnail> thumbs{};
    };
    std::map<std::string, Entry> files{};

    Entry& get(const std::string& file) {
        Entry& e = files[file];
        const std::filesystem::path path(std::u8string(file.begin(), file.end()));
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        const std::int64_t stamp = ec ? 0 : t.time_since_epoch().count();
        if (e.stamp != stamp) {
            e = Entry{};
            e.stamp = stamp;
            auto src = xref::loadSource(path);
            if (src)
                e.source = std::make_shared<xref::Source>(std::move(src.value()));
            else
                e.error = src.error().message;
        }
        return e;
    }
    const Thumbnail* thumb(Entry& e, const std::string& block, int& budget) {
        if (!e.source || !e.source->doc)
            return nullptr;
        auto it = e.thumbs.find(block);
        if (it != e.thumbs.end())
            return &it->second;
        if (budget <= 0)
            return nullptr;
        --budget;
        const BlockDef* def = e.source->doc->blockByName(block);
        return &(e.thumbs[block] = def ? thumbnailOfBlock(*e.source->doc, def->id) : Thumbnail{});
    }
};

SourceCache& sources() {
    static SourceCache c;
    return c;
}

void rememberDrawing(cmd::IHost* host, const std::string& file) {
    if (host)
        host->setSetting(
            ui::kDrawingHistorySetting,
            ui::pushPathHistory(host->setting(ui::kDrawingHistorySetting).value_or(""), file));
}

struct OtherTab {
    std::string path{};
    std::string open{};  // the file shown
    std::string filter{};
    std::string selected{};
};

void drawOtherTab(ui::PanelContext& ctx) {
    static OtherTab st;
    if (ctx.host)
        if (auto f = ctx.host->setting("tmp.blocks.otherfile"); f && !f->empty()) {
            st.path = st.open = *f;
            ctx.host->setSetting("tmp.blocks.otherfile", "");
        }
    const auto history = ui::parsePathHistory(
        ctx.host ? ctx.host->setting(ui::kDrawingHistorySetting).value_or("") : std::string());
    ImGui::SetNextItemWidth(-FLT_MIN - 70);
    const bool enter = ImGui::InputTextWithHint("##otherpath", "Drawing file (.tbcad, .dxf)",
                                                &st.path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(history.empty());
    if (ImGui::Button("Recent", ImVec2(-FLT_MIN, 0)))
        ImGui::OpenPopup("##otherhistory");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##otherhistory")) {
        for (const std::string& h : history)
            if (ImGui::MenuItem(h.c_str())) {
                st.path = h;
                st.open = h;
            }
        ImGui::EndPopup();
    }
    if (enter && !st.path.empty())
        st.open = st.path;
    if (st.open.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Type the path of another drawing and press Enter to see its blocks. Double-click or "
            "drag one to copy it into this drawing and insert it.");
        return;
    }
    SourceCache::Entry& e = sources().get(st.open);
    if (!e.source) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", e.error.c_str());
        return;
    }
    if (st.open == st.path)
        rememberDrawing(ctx.host, st.open);
    st.path = st.open;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##otherfilter", "Filter blocks", &st.filter);
    const auto rows = blockRows(*e.source->doc, st.filter);
    std::vector<const BlockRow*> shown;
    for (const BlockRow& r : rows) {
        const BlockDef* def = e.source->doc->block(r.id);
        if (def && def->xrefPath.empty())
            shown.push_back(&r);
    }
    if (shown.empty()) {
        ImGui::TextDisabled("The drawing has no blocks");
        return;
    }
    ImGui::BeginChild("##othertiles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    int budget = 4;
    float used = 0;
    for (const BlockRow* r : shown) {
        const Thumbnail* thumb = sources().thumb(e, r->name, budget);
        const bool here = ctx.doc->blockByName(r->name) != nullptr;
        TileResult t = drawTile(r->name.c_str(), r->name, here ? "in drawing" : "", thumb,
                                st.selected == r->name);
        dragSource(st.open, r->name, thumb);
        if (t.clicked)
            st.selected = r->name;
        if (t.doubleClicked)
            placeBlock(ctx, r->name, st.open);
        if (ImGui::BeginPopupContextItem("##othermenu")) {
            if (ImGui::MenuItem("Insert"))
                placeBlock(ctx, r->name, st.open);
            if (ImGui::MenuItem("Copy into this drawing") && ctx.runCommand) {
                ctx.runCommand("CONTENTIMPORT");
                ctx.runCommand(st.open);
                ctx.runCommand("Block");
                ctx.runCommand(r->name);
            }
            ImGui::EndPopup();
        }
        nextTile(used);
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click or drag to insert; the block is copied in first");
}

// ---- Recent tab --------------------------------------------------------------------------------

void drawRecentTab(ui::PanelContext& ctx) {
    static DrawingTab here;  // thumbnails of this drawing's blocks
    static LibraryThumbnails files;
    if (here.doc != ctx.doc) {
        here = DrawingTab{};
        here.doc = ctx.doc;
    }
    const auto list =
        parseRecent(ctx.host ? ctx.host->setting("block.recent").value_or("") : std::string());
    if (list.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Blocks you insert show up here");
        return;
    }
    if (ImGui::SmallButton("Clear") && ctx.host) {
        ctx.host->setSetting("block.recent", "");
        return;
    }
    std::vector<LibraryItem> wholeFiles;
    for (const RecentBlock& r : list)
        if (r.whole)
            wholeFiles.push_back(
                {std::filesystem::path(std::u8string(r.file.begin(), r.file.end())), r.name});
    files.loadNext(wholeFiles);
    ImGui::BeginChild("##recenttiles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    int budget = 4;
    float used = 0;
    int i = 0;
    for (const RecentBlock& r : list) {
        const Thumbnail* thumb = nullptr;
        std::string caption;
        if (r.whole) {
            thumb = files.find(std::filesystem::path(std::u8string(r.file.begin(), r.file.end())));
            caption = "file";
        } else if (const BlockDef* def = ctx.doc->blockByName(r.name)) {
            auto& slot = here.cache[def->id];
            if (slot.first != ctx.doc->revision() + 1 && budget-- > 0)
                slot = {ctx.doc->revision() + 1, thumbnailOfBlock(*ctx.doc, def->id)};
            thumb = slot.first ? &slot.second : nullptr;
        } else if (!r.file.empty()) {
            thumb = sources().thumb(sources().get(r.file), r.name, budget);
            caption = std::filesystem::path(std::u8string(r.file.begin(), r.file.end()))
                          .filename()
                          .string();
        } else {
            caption = "not here";
        }
        const std::string id = std::to_string(i++) + r.name;
        TileResult t = drawTile(id.c_str(), r.name, caption, thumb, false);
        if (ImGui::IsItemHovered() && !r.file.empty())
            ImGui::SetTooltip("%s\n%s", r.name.c_str(), r.file.c_str());
        const bool placeable = r.whole || ctx.doc->blockByName(r.name) || !r.file.empty();
        if (placeable && !r.whole)
            dragSource(ctx.doc->blockByName(r.name) ? std::string() : r.file, r.name, thumb);
        if (t.doubleClicked && placeable)
            placeBlock(ctx, r.name, r.file, r.whole);
        nextTile(used);
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click or drag to insert again");
}

// ---- Library tab -------------------------------------------------------------------------------

struct LibraryTab {
    std::string folder{};
    bool folderRead = false;
    std::string filter{};
    std::vector<LibraryItem> items{};
    std::string scannedFolder{};
    std::string scannedFilter{};
    double scannedAt = -1e9;
    LibraryThumbnails thumbs{};
    std::filesystem::path selected{};
};

void drawLibraryTab(ui::PanelContext& ctx) {
    static LibraryTab st;
    if (!st.folderRead) {
        if (ctx.host)
            if (auto f = ctx.host->setting(kLibrarySetting))
                st.folder = *f;
        st.folderRead = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##libfolder", "Folder with .tbcad / .dxf files", &st.folder) &&
        ctx.host)
        ctx.host->setSetting(kLibrarySetting, st.folder);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##libfilter", "Filter files", &st.filter);

    // Rescan when the folder or filter changed, and now and then for new files.
    const double now = ImGui::GetTime();
    if (st.scannedFolder != st.folder || st.scannedFilter != st.filter || now - st.scannedAt > 3) {
        st.items = scanLibrary(
            std::filesystem::path(std::u8string(st.folder.begin(), st.folder.end())), st.filter);
        st.scannedFolder = st.folder;
        st.scannedFilter = st.filter;
        st.scannedAt = now;
    }
    if (st.folder.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Type the folder where you keep drawings you reuse as blocks.");
        return;
    }
    if (st.items.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No .tbcad or .dxf files found");
        return;
    }
    st.thumbs.loadNext(st.items);
    ImGui::BeginChild("##libtiles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    float used = 0;
    for (const LibraryItem& item : st.items) {
        const std::string path = utf8(item.path);
        TileResult t = drawTile(path.c_str(), item.name,
                                item.path.extension().string() == ".dxf" ? "dxf" : "tbcad",
                                st.thumbs.find(item.path), st.selected == item.path);
        if (t.clicked)
            st.selected = item.path;
        if (t.doubleClicked)
            placeBlock(ctx, item.name, path, true);
        if (ImGui::BeginPopupContextItem("##libmenu")) {
            if (ImGui::MenuItem("Insert as block"))
                placeBlock(ctx, item.name, path, true);
            if (ImGui::MenuItem("Show its blocks") && ctx.host) {
                rememberDrawing(ctx.host, path);
                ctx.host->setSetting("tmp.blocks.tab", "other");
            }
            if (ImGui::MenuItem("Attach as xref") && ctx.runCommand) {
                ctx.runCommand("XATTACH");
                ctx.runCommand(path);
            }
            ImGui::EndPopup();
        }
        nextTile(used);
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click to insert; right-click to attach as xref");
}

// The block palette: blocks of this drawing, recently inserted blocks, blocks of another drawing
// and a library folder, as tiles with thumbnails, plus the options the palette inserts with.
void drawBlockPanel(ui::PanelContext& ctx) {
    if (!ctx.doc) {
        ImGui::TextUnformatted("No drawing open");
        return;
    }
    drawOptions(ctx);
    // "Show its blocks" in the Library tab switches to Other Drawing.
    std::string jump;
    if (ctx.host)
        if (auto t = ctx.host->setting("tmp.blocks.tab"); t && !t->empty()) {
            jump = *t;
            ctx.host->setSetting("tmp.blocks.tab", "");
        }
    if (ImGui::BeginTabBar("##blocktabs")) {
        if (ImGui::BeginTabItem("Current Drawing")) {
            drawDrawingTab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recent")) {
            drawRecentTab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Other Drawing", nullptr,
                                jump == "other" ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (jump == "other" && ctx.host) {
                // Open the drawing the Library tab asked for.
                const auto h = ui::parsePathHistory(
                    ctx.host->setting(ui::kDrawingHistorySetting).value_or(""));
                if (!h.empty())
                    ctx.host->setSetting("tmp.blocks.otherfile", h.front());
            }
            drawOtherTab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Library")) {
            drawLibraryTab(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// The Block contextual tab: shown over the drawing while only block references are selected.
void drawBlockContextTab(ui::PanelContext& ctx) {
    if (!ctx.doc || !ctx.runCommand || !ctx.selection || ctx.selection->empty())
        return;
    // Names of the blocks selected, for the label.
    std::string names;
    std::size_t count = 0;
    bool xref = false;
    for (EntityId id : *ctx.selection) {
        const Entity* e = ctx.doc->find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        const BlockDef* def = ins ? ctx.doc->block(ins->block) : nullptr;
        if (!def)
            continue;
        xref = xref || !def->xrefPath.empty();
        if (names.find(def->name) == std::string::npos && count < 3) {
            names += (names.empty() ? "" : ", ") + def->name;
            ++count;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", names.c_str());
    const bool one = ctx.selection->size() == 1;
    struct Tool {
        const char* label;
        const char* command;
        bool enabled;
    };
    const Tool tools[] = {{"Edit block", "BEDIT", one && !xref},
                          {"Edit in place", "REFEDIT", one},
                          {"Attributes", "ATTEDIT", one},
                          {"Sync attributes", "ATTSYNC", !xref},
                          {"Replace", "BLOCKREPLACE", !xref},
                          {"Clip", "XCLIP", true},
                          {"Explode", "EXPLODE", !xref},
                          {"Count", "BLOCKCOUNT", true},
                          {"Blocks palette", "BLOCKSPALETTE", true}};
    for (const Tool& t : tools) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!t.enabled);
        if (ImGui::SmallButton(t.label)) {
            ctx.runCommand(t.command);
            if (std::string_view(t.command) == "BEDIT") {  // BEDIT asks for the block itself
                ctx.runCommand("Name");
                ctx.runCommand(names);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", t.command);
    }
}

// The Define Attribute dialog: collects what -ATTDEF asks on the command line, stores it in the
// "attdef.*" settings and runs ATTDEF again, which then only needs the start point.
struct AttDefForm {
    std::string tag, prompt, defaultValue;
    double height = 2.5, rotation = 0;
    bool invisible = false, constant = false, verify = false, preset = false;
    std::string error;
    bool loaded = false;
};

// Label on the left, box filling the rest of the row (matches the Properties panel).
std::string leftLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(-1);
    return std::string("##") + label;
}

// True when Enter was pressed in the field just drawn (the form's default action).
bool enterPressed() {
    return ImGui::IsItemDeactivated() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
}

void drawAttDefPanel(ui::PanelContext& ctx) {
    static AttDefForm f;
    if (!ctx.host)
        return;
    if (!f.loaded) {
        if (auto h = ctx.host->setting("attdef.height"))
            if (double v = std::strtod(h->c_str(), nullptr); v > 0)
                f.height = v;
        f.loaded = true;
    }
    bool ok = false;
    ImGui::TextUnformatted("Attribute");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputText(leftLabel("Tag").c_str(), &f.tag, ImGuiInputTextFlags_AutoSelectAll);
    ok = enterPressed() || ok;
    ImGui::InputText(leftLabel("Prompt").c_str(), &f.prompt, ImGuiInputTextFlags_AutoSelectAll);
    ok = enterPressed() || ok;
    ImGui::InputText(leftLabel("Default value").c_str(), &f.defaultValue,
                     ImGuiInputTextFlags_AutoSelectAll);
    ok = enterPressed() || ok;
    ImGui::Separator();
    ImGui::TextUnformatted("Mode");
    ImGui::Checkbox("Invisible", &f.invisible);
    ImGui::SameLine();
    ImGui::Checkbox("Constant", &f.constant);
    ImGui::SameLine();
    ImGui::Checkbox("Verify", &f.verify);
    ImGui::SameLine();
    ImGui::Checkbox("Preset", &f.preset);
    ImGui::Separator();
    ImGui::InputDouble(leftLabel("Text height").c_str(), &f.height, 0, 0, "%.6g");
    ok = enterPressed() || ok;
    ImGui::InputDouble(leftLabel("Rotation (deg)").c_str(), &f.rotation, 0, 0, "%.6g");
    ok = enterPressed() || ok;
    if (!f.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", f.error.c_str());
    if (ImGui::Button("OK, pick the start point"))
        ok = true;
    if (ok) {
        const std::string tag = blocks::normalizeTag(f.tag);
        if (auto valid = blocks::validateTag(tag); !valid.ok()) {
            f.error = valid.error().message;
        } else if (!(f.height > 0)) {
            f.error = "The text height must be above zero.";
        } else {
            f.error.clear();
            auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
            ctx.host->setSetting("tmp.attdef.tag", tag);
            ctx.host->setSetting("tmp.attdef.prompt", f.prompt);
            ctx.host->setSetting("tmp.attdef.default", f.defaultValue);
            ctx.host->setSetting("tmp.attdef.invisible", flag(f.invisible));
            ctx.host->setSetting("tmp.attdef.constant", flag(f.constant));
            ctx.host->setSetting("tmp.attdef.verify", flag(f.verify));
            ctx.host->setSetting("tmp.attdef.preset", flag(f.preset));
            ctx.host->setSetting("attdef.height", std::to_string(f.height));
            ctx.host->setSetting("tmp.attdef.rotation", std::to_string(f.rotation));
            ctx.host->setSetting("tmp.attdef.go", "1");
            ctx.host->closePanel("attdef");
            if (ctx.cancelCommand)
                ctx.cancelCommand();
            if (ctx.runCommand)
                ctx.runCommand("ATTDEF");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                    ImGui::IsKeyPressed(ImGuiKey_Escape)))
        ctx.host->closePanel("attdef");
}

// The attribute values INSERT asks for after the block is placed: one field per attribute. OK
// stores "tag<US>value<RS>..." in tmp.insatt.result and presses Enter for INSERT; Cancel ends it.
void drawAttValuesPanel(ui::PanelContext& ctx) {
    static std::string shownFor;
    static std::vector<std::array<std::string, 3>> rows;  // tag, label, value
    if (!ctx.host)
        return;
    const std::string fields = ctx.host->setting("tmp.insatt.fields").value_or("");
    if (fields.empty() || !ctx.commandActive) {
        ImGui::TextDisabled("No attribute values are being asked for.");
        return;
    }
    if (fields != shownFor) {
        shownFor = fields;
        rows.clear();
        std::size_t at = 0;
        while (at < fields.size()) {
            const std::size_t end = fields.find('\x1e', at);
            const std::string rec = fields.substr(at, end - at);
            const std::size_t a = rec.find('\x1f');
            const std::size_t b = a == std::string::npos ? a : rec.find('\x1f', a + 1);
            if (b != std::string::npos)
                rows.push_back({rec.substr(0, a), rec.substr(a + 1, b - a - 1), rec.substr(b + 1)});
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
    }
    bool okKey = false;
    ImGui::Text("Attributes of %s", ctx.host->setting("tmp.insatt.block").value_or("").c_str());
    ImGui::Separator();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i == 0 && ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText(leftLabel(rows[i][1].c_str()).c_str(), &rows[i][2],
                         ImGuiInputTextFlags_AutoSelectAll);
        okKey = enterPressed() || okKey;
        ImGui::PopID();
    }
    if (ImGui::Button("OK") || okKey) {
        std::string result;
        for (const auto& r : rows)
            result += r[0] + "\x1f" + r[2] + "\x1e";
        ctx.host->setSetting("tmp.insatt.result", result);
        shownFor.clear();
        if (ctx.runCommand)
            ctx.runCommand("");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                    ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        shownFor.clear();
        if (ctx.cancelCommand)
            ctx.cancelCommand();
    }
}

}  // namespace

#endif

void registerBlockPanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("blocks", "Blocks", drawBlockPanel, false);
    ui.addContextualTab("block", "Block", {EntityKind::Insert}, drawBlockContextTab);
    ui.addPanel("attdef", "Define Attribute", drawAttDefPanel, false);
    ui.addPanel("attvalues", "Attribute Values", drawAttValuesPanel, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
