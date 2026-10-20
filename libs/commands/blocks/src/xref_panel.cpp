#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "commands.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "tbcad/xref/xref.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

struct XrefPanelState {
    BlockId selected{};
    std::string relink{};
    BlockId relinkFor{};
    bool tree = false;
};

XrefPanelState& state() {
    static XrefPanelState s;
    return s;
}

// Size and date of a referenced file, read at most every couple of seconds: the panel draws every
// frame and a file on a network share is slow to ask.
struct FileFacts {
    std::string size{};
    std::string date{};
    double checkedAt = -100;
};

std::string sizeText(std::uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024)
        std::snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(bytes));
    else if (bytes < 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string dateText(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec)
        return {};
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

const FileFacts& factsOf(const std::filesystem::path& file) {
    static std::map<std::filesystem::path, FileFacts> cache;
    FileFacts& f = cache[file];
    const double now = ImGui::GetTime();
    if (now - f.checkedAt < 2.0)
        return f;
    f.checkedAt = now;
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
    f.size = ec ? std::string() : sizeText(bytes);
    f.date = ec ? std::string() : dateText(file);
    return f;
}

const char* stateLabel(xref::State s) {
    switch (s) {
        case xref::State::UpToDate:
            return "Loaded";
        case xref::State::Changed:
            return "Needs reload";
        case xref::State::Missing:
            return "Not found";
        case xref::State::Unloaded:
            return "Unloaded";
        case xref::State::NotXref:
            break;
    }
    return "";
}

const char* typeLabel(const BlockDef& d) {
    if (underlay::isImagePath(d.xrefPath))
        return "Image";
    if (underlay::isPdfPath(d.xrefPath))
        return "PDF";
    return d.xrefOverlay ? "Overlay" : "Attach";
}

bool isDrawing(const BlockDef& d) {
    return !underlay::isImagePath(d.xrefPath) && !underlay::isPdfPath(d.xrefPath);
}

const char* pathTypeLabel(std::string_view stored) {
    switch (xref::pathTypeOf(stored)) {
        case xref::PathType::Absolute:
            return "Full path";
        case xref::PathType::Relative:
            return "Relative path";
        case xref::PathType::None:
            return "No path";
    }
    return "";
}

// Runs a command that asks for the xref name first, then its further answers.
void runOn(ui::PanelContext& ctx, const char* command, const std::string& name,
           std::initializer_list<const char*> answers = {}) {
    if (!ctx.runCommand)
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    ctx.runCommand(command);
    ctx.runCommand(name);
    for (const char* a : answers)
        ctx.runCommand(a);
}

// The actions on one reference, for the right-click menu of its row. True when one ran: it may
// have rewritten or removed blocks.
bool rowMenu(ui::PanelContext& ctx, const BlockDef& d) {
    const std::string name = d.name;
    const bool drawing = isDrawing(d);
    const bool overlay = d.xrefOverlay;
    const std::string stored = d.xrefPath;
    bool ran = false;
    const auto run = [&](const char* command, std::initializer_list<const char*> answers = {}) {
        runOn(ctx, command, name, answers);
        ran = true;
    };
    if (drawing && ImGui::MenuItem("Open"))
        run("XOPEN");
    if (ImGui::MenuItem("Reload"))
        run("XRELOAD");
    if (ImGui::MenuItem("Unload"))
        run("XUNLOAD");
    if (ImGui::MenuItem("Detach"))
        run("XDETACH");
    if (drawing && ImGui::BeginMenu("Bind")) {
        if (ImGui::MenuItem("Bind (keep its layers apart)"))
            run("XBIND", {"Bind"});
        if (ImGui::MenuItem("Insert (merge layers)"))
            run("XBIND", {"Insert"});
        ImGui::EndMenu();
    }
    if (drawing && ImGui::BeginMenu("Reference type")) {
        if (ImGui::MenuItem("Attach", nullptr, !overlay) && overlay)
            run("XREFTYPE", {"Attach"});
        if (ImGui::MenuItem("Overlay", nullptr, overlay) && !overlay)
            run("XREFTYPE", {"Overlay"});
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Path")) {
        const xref::PathType now = xref::pathTypeOf(stored);
        if (ImGui::MenuItem("Relative", nullptr, now == xref::PathType::Relative))
            run("XPATH", {"Relative"});
        if (ImGui::MenuItem("Full path", nullptr, now == xref::PathType::Absolute))
            run("XPATH", {"Absolute"});
        if (ImGui::MenuItem("No path", nullptr, now == xref::PathType::None))
            run("XPATH", {"None"});
        ImGui::EndMenu();
    }
    return ran;
}

struct RowEnv {
    ui::PanelContext& ctx;
    const Document& doc;
    const std::filesystem::path& hostFile;
    XrefPanelState& st;
    // Set by a menu action: the menu ran a command that may have removed rows.
    bool acted = false;
};

// One row of the table; in the tree view its nested references follow as child rows.
void drawRow(RowEnv& env, BlockId id, int depth) {
    const BlockDef* d = env.doc.block(id);
    if (!d || env.acted)
        return;
    // Copies: a command from the row's menu may rewrite or remove the block.
    const std::string name = d->name;
    const std::string stored = d->xrefPath;
    const char* type = typeLabel(*d);
    const xref::State s = xref::state(env.doc, id, env.hostFile);
    const FileFacts& facts = factsOf(xref::resolvePath(stored, env.hostFile));
    std::vector<BlockId> kids;
    if (env.st.tree && depth < 16)
        kids = xref::children(env.doc, id);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(static_cast<int>(id.value));
    ImGui::PushID(depth);
    bool open = false;
    if (env.st.tree) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns |
                                   ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
        if (kids.empty())
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (env.st.selected == id)
            flags |= ImGuiTreeNodeFlags_Selected;
        open = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            env.st.selected = id;
    } else if (ImGui::Selectable(name.c_str(), env.st.selected == id,
                                 ImGuiSelectableFlags_SpanAllColumns)) {
        env.st.selected = id;
    }
    if (ImGui::BeginPopupContextItem("##row")) {
        env.st.selected = id;
        if (rowMenu(env.ctx, *d))
            env.acted = true;
        ImGui::EndPopup();
    }
    ImGui::TableSetColumnIndex(1);
    if (s == xref::State::Missing)
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", stateLabel(s));
    else if (s == xref::State::Changed)
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "%s", stateLabel(s));
    else
        ImGui::TextUnformatted(stateLabel(s));
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(facts.size.c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(type);
    ImGui::TableSetColumnIndex(4);
    ImGui::TextUnformatted(facts.date.c_str());
    ImGui::TableSetColumnIndex(5);
    ImGui::TextUnformatted(stored.c_str());
    ImGui::PopID();
    ImGui::PopID();
    if (open && !kids.empty()) {
        for (BlockId k : kids)
            drawRow(env, k, depth + 1);
        ImGui::TreePop();
    }
}

void drawXrefPanel(ui::PanelContext& ctx) {
    if (!ctx.doc)
        return;
    XrefPanelState& st = state();
    const Document& doc = *ctx.doc;
    std::filesystem::path hostFile;
    if (ctx.host)
        if (auto f = ctx.host->setting("document.file"))
            hostFile = std::filesystem::path(std::u8string(f->begin(), f->end()));
    const std::vector<BlockId> ids = xref::xrefs(doc);

    if (ImGui::Button("Attach...") && ctx.runCommand) {
        if (ctx.cancelCommand)
            ctx.cancelCommand();
        ctx.runCommand("XATTACH");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload all") && ctx.runCommand && !ids.empty()) {
        if (ctx.cancelCommand)
            ctx.cancelCommand();
        ctx.runCommand("XRELOAD");
        ctx.runCommand("All");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("List", !st.tree))
        st.tree = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Tree", st.tree))
        st.tree = true;
    ImGui::Separator();
    if (ids.empty()) {
        ImGui::TextDisabled("No external references in this drawing.");
        return;
    }

    const BlockDef* sel = st.selected ? doc.block(st.selected) : nullptr;
    if (sel && sel->xrefPath.empty())
        sel = nullptr;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 8.5f;
    bool acted = false;
    if (ImGui::BeginTable("##xrefs", 6,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders,
                          ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Saved path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        RowEnv env{ctx, doc, hostFile, st};
        if (st.tree) {
            // Roots: references no other reference nests; the rest hang below their parents.
            std::set<BlockId> nested;
            for (BlockId id : ids)
                for (BlockId k : xref::children(doc, id))
                    nested.insert(k);
            for (BlockId id : ids)
                if (!nested.count(id))
                    drawRow(env, id, 0);
        } else {
            for (BlockId id : ids)
                drawRow(env, id, 0);
        }
        acted = env.acted;
        ImGui::EndTable();
    }
    if (acted)
        sel = st.selected ? doc.block(st.selected) : nullptr;
    if (sel && sel->xrefPath.empty())
        sel = nullptr;

    if (!sel) {
        ImGui::TextDisabled("Select a reference to see its details; right-click it for more.");
        return;
    }
    // Copies: the buttons below run commands that rewrite or remove this block, so `sel` must not
    // be used after them.
    const std::string name = sel->name;
    const BlockId selId = sel->id;
    const std::string stored = sel->xrefPath;
    const bool drawing = isDrawing(*sel);
    const bool overlay = sel->xrefOverlay;
    const xref::State s = xref::state(doc, selId, hostFile);
    const std::filesystem::path found = xref::resolvePath(stored, hostFile);

    if (ImGui::BeginTable("##xrefdetails", 2, ImGuiTableFlags_SizingFixedFit)) {
        const auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        const std::u8string foundU8 = found.u8string();
        row("Status", stateLabel(s));
        row("Found at", s == xref::State::Missing ? std::string("(not found)")
                                                  : std::string(foundU8.begin(), foundU8.end()));
        row("Saved path", stored + "  (" + pathTypeLabel(stored) + ")");
        row("Used", std::to_string(xref::useCount(doc, selId)) + " times");
        ImGui::EndTable();
    }

    if (drawing && ImGui::Button("Open"))
        runOn(ctx, "XOPEN", name);
    if (drawing)
        ImGui::SameLine();
    if (ImGui::Button("Reload"))
        runOn(ctx, "XRELOAD", name);
    ImGui::SameLine();
    if (ImGui::Button("Unload"))
        runOn(ctx, "XUNLOAD", name);
    ImGui::SameLine();
    if (ImGui::Button("Detach"))
        runOn(ctx, "XDETACH", name);
    if (drawing) {
        if (ImGui::Button("Bind"))
            runOn(ctx, "XBIND", name, {"Bind"});
        ImGui::SameLine();
        if (ImGui::Button("Insert (merge layers)"))
            runOn(ctx, "XBIND", name, {"Insert"});
        ImGui::SameLine();
        if (ImGui::Button(overlay ? "Make attachment" : "Make overlay"))
            runOn(ctx, "XREFTYPE", name, {overlay ? "Attach" : "Overlay"});
    }
    if (ImGui::BeginCombo("Path type", pathTypeLabel(stored))) {
        for (const auto& [label, answer] :
             {std::pair{"Relative path", "Relative"}, std::pair{"Full path", "Absolute"},
              std::pair{"No path", "None"}})
            if (ImGui::Selectable(label))
                runOn(ctx, "XPATH", name, {answer});
        ImGui::EndCombo();
    }
    if (st.relinkFor != selId) {
        st.relinkFor = selId;
        st.relink.clear();
    }
    if (s == xref::State::Missing)
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "File not found. Give its new location:");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##relink", "New file for this reference", &st.relink);
    ImGui::SameLine();
    if (ImGui::Button("Relink") && !st.relink.empty()) {
        runOn(ctx, "XRELINK", name, {st.relink.c_str()});
        st.relink.clear();
    }
}

}  // namespace

#endif

void registerXrefPanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("xrefs", "External References", drawXrefPanel, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
