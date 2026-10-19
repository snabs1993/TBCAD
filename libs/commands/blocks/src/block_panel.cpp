#include <array>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "commands.h"
#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/block_cmds/block_list.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/blocks/tbcad_blocks.h"

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
        TileResult t = drawTile(r.name.c_str(), r.name, caption,
                                slot.first ? &slot.second : nullptr, st.selected == r.id);
        if (t.clicked)
            st.selected = r.id;
        if (t.doubleClicked && ctx.runCommand)
            ctx.runCommand("INSERT " + r.name);
        if (ImGui::BeginPopupContextItem("##tilemenu")) {
            st.selected = r.id;
            if (ImGui::MenuItem("Insert") && ctx.runCommand)
                ctx.runCommand("INSERT " + r.name);
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
                    for (const char* t : {"Relative", "Absolute", "None"})
                        if (ImGui::MenuItem(t)) {
                            ctx.runCommand("XPATH");
                            ctx.runCommand(r.name);
                            ctx.runCommand(t);
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
        used += kTile + ImGui::GetStyle().ItemSpacing.x;
        if (used + kTile > ImGui::GetContentRegionAvail().x)
            used = 0;
        else
            ImGui::SameLine();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click a block to insert it; right-click for more");
}

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
        auto insert = [&] {
            if (!ctx.runCommand || !ctx.host)
                return;
            ctx.host->setSetting("block.lastinsert", path);
            ctx.runCommand("INSERT");
            ctx.runCommand("");  // take the file as the answer
        };
        if (t.doubleClicked)
            insert();
        if (ImGui::BeginPopupContextItem("##libmenu")) {
            if (ImGui::MenuItem("Insert as block"))
                insert();
            if (ImGui::MenuItem("Attach as xref") && ctx.runCommand) {
                ctx.runCommand("XATTACH");
                ctx.runCommand(path);
            }
            ImGui::EndPopup();
        }
        used += kTile + ImGui::GetStyle().ItemSpacing.x;
        if (used + kTile > ImGui::GetContentRegionAvail().x)
            used = 0;
        else
            ImGui::SameLine();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click to insert; right-click to attach as xref");
}

// The block palette: blocks of the drawing and a library folder, as tiles with thumbnails.
void drawBlockPanel(ui::PanelContext& ctx) {
    if (!ctx.doc) {
        ImGui::TextUnformatted("No drawing open");
        return;
    }
    if (ImGui::BeginTabBar("##blocktabs")) {
        if (ImGui::BeginTabItem("Drawing")) {
            drawDrawingTab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Library")) {
            drawLibraryTab(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
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
        if (auto ok = blocks::validateTag(tag); !ok.ok()) {
            f.error = ok.error().message;
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
    ui.addPanel("attdef", "Define Attribute", drawAttDefPanel, false);
    ui.addPanel("attvalues", "Attribute Values", drawAttValuesPanel, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
