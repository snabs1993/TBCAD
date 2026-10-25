#include "gui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

#include "icons.h"
#include "key_map.h"
#include "tbcad/core/color.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/mcp/png.h"
#include "tbcad/platform/clipboard_picture.h"
#include "tbcad/platform/crash.h"
#include "tbcad/platform/external_link.h"
#include "tbcad/platform/file_dialog.h"
#include "tbcad/platform/paths.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/customize.h"
#include "tbcad/ui/drag_payload.h"
#include "tbcad/ui/radial_wheel.h"
#include "tbcad/ui/selection.h"
#include "tbcad/ui/status_items.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace tbcad::app {

namespace {

constexpr ImGuiID kDockId = 0x7BC4D001u;

const std::vector<platform::FileFilter>& openFilters() {
    static const std::vector<platform::FileFilter> f = {{"All drawings", "*.tbcad;*.dxf;*.dwg"},
                                                        {"TBCAD drawings", "*.tbcad"},
                                                        {"DXF drawings", "*.dxf"},
                                                        {"DWG drawings (read-only)", "*.dwg"}};
    return f;
}

const std::vector<platform::FileFilter>& drawingFilters(bool dxfFirst = false) {
    static const std::vector<platform::FileFilter> tbcadFirst = {{"TBCAD drawings", "*.tbcad"},
                                                                 {"DXF drawings", "*.dxf"},
                                                                 {"All drawings", "*.tbcad;*.dxf"}};
    static const std::vector<platform::FileFilter> dxfFirstList = {
        {"DXF drawings", "*.dxf"},
        {"TBCAD drawings", "*.tbcad"},
        {"All drawings", "*.tbcad;*.dxf"}};
    return dxfFirst ? dxfFirstList : tbcadFirst;
}

// A tree of registered menu items ("Draw/Circle/Center, Radius").
struct MenuNode {
    std::string name;
    const ui::MenuItemInfo* item = nullptr;  // leaf
    std::vector<MenuNode> children;
};

void insertItem(MenuNode& root, const ui::MenuItemInfo& item, std::string_view rest) {
    auto slash = rest.find('/');
    std::string head(rest.substr(0, slash));
    if (slash == std::string_view::npos) {
        root.children.push_back({head, &item, {}});
        return;
    }
    auto it = std::find_if(root.children.begin(), root.children.end(),
                           [&](const MenuNode& n) { return !n.item && n.name == head; });
    if (it == root.children.end()) {
        root.children.push_back({head, nullptr, {}});
        it = root.children.end() - 1;
    }
    insertItem(*it, item, rest.substr(slash + 1));
}

}  // namespace

Gui::Gui(ui::AppCore& core, ui::UiRegistry& registry, GLFWwindow* window)
    : core_(core),
      registry_(registry),
      window_(window),
      settings_(SettingsDialog::Hooks{
          [this] { return static_cast<int>(themeMode_); },
          [this](int mode) { setThemeMode(static_cast<ui::ThemeMode>(mode)); },
          [this] { setDpiScale(dpiScale_); },
          [this](const std::string& action) { return actionLabel(action); },
          [this] { cursorHud_ = flag("hud.enabled", true); }}),
      workspaces_(core.settingsFolder() / "workspaces") {
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    dpiScale_ = std::max(1.0f, sx);
    recoveryCount_ = core_.pendingRecoveries().size();
    recovery_ = core_.pendingRecovery();
    {
        auto crashes = platform::pendingCrashReports(core_.paths().crashDir());
        if (!crashes.empty())
            crashReport_ = platform::pathToUtf8(crashes.back());
    }
    openRecoveryPrompt_ = recovery_.has_value() || !crashReport_.empty();
    anyActivity_ = true;
    themeMode_ = ui::parseThemeMode(core_.settings().getString("ui.theme", "dark"));
    theme_ = ui::themeFor(themeMode_, systemPrefersDark());
    core_.setTheme(theme_);
    core_.setDialogsAvailable(true);
    {
        // The starter symbols (doors, windows, furniture) that ship with the program are the
        // block library until the user picks another folder.
        const std::string shipped = findBesideExecutable("assets/blocks");
        std::error_code ec;
        const auto current = core_.setting("block.library");
        if (!shipped.empty() &&
            std::filesystem::is_directory(platform::pathFromUtf8(shipped), ec) &&
            (!current || current->empty() ||
             !std::filesystem::is_directory(platform::pathFromUtf8(*current), ec)))
            core_.setSetting("block.library", shipped);
    }
    cursorHud_ = flag("hud.enabled", true);
    for (const auto& p : registry_.panels())
        panelOpen_[p.id] = p.visibleByDefault;
    // The default dock sizes changed (a command bar of about four rows); a layout saved by
    // an older build is replaced once.
    if (core_.settings().getString("ui.dockLayout", "") != "v3") {
        rebuildDock_ = true;
        core_.settings().setString("ui.dockLayout", "v3");
    }
    // The workspace used last time. Only the panels are applied here: the docking layout comes
    // back from imgui.ini by itself.
    workspaceName_ = core_.settings().getString("workspace.current", "");
    if (!workspaceName_.empty()) {
        if (auto ws = workspaces_.load(workspaceName_)) {
            for (auto& [id, on] : panelOpen_) {
                auto it = ws->panels.find(id);
                on = it != ws->panels.end() && it->second;
            }
            showCommandLine_ = ws->commandLine;
            if (!ws->panels.count("properties"))
                panelOpen_["properties"] = ws->properties;
        } else {
            workspaceName_.clear();
        }
    }
    // Text layout finds its fonts and the Thai word list next to the executable.
    if (std::string f = findAsset("fonts/Sarabun-Regular.ttf"); !f.empty())
        text::fonts().setBundledDirectory(std::filesystem::path(f).parent_path());
    if (std::string d = findAsset("dictionaries/thai-words.txt"); !d.empty())
        text::setDictionaryDirectory(std::filesystem::path(d).parent_path());
    // The IME hook positions the candidate window on this native window.
    ImGui::GetMainViewport()->PlatformHandleRaw = nativeHandle();
    installImeHook(window);
}

namespace {

// Shows the Markdown of the help page as plain text: headings in the accent colour, bullets
// with a dot, and the **bold** and `code` markers removed.
void drawMarkdownLite(const std::string& text) {
    auto clean = [](std::string line) {
        for (const char* mark : {"**", "`"}) {
            const std::string m = mark;
            for (size_t at; (at = line.find(m)) != std::string::npos;)
                line.erase(at, m.size());
        }
        return line;
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t level = 0;
        while (level < line.size() && line[level] == '#')
            ++level;
        if (level > 0 && level < line.size() && line[level] == ' ') {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::TextWrapped("%s", clean(line.substr(level + 1)).c_str());
            ImGui::PopStyleColor();
            if (level <= 2)
                ImGui::Separator();
        } else if (line.rfind("- ", 0) == 0) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", clean(line.substr(2)).c_str());
        } else if (line.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
        } else {
            ImGui::TextWrapped("%s", clean(line).c_str());
        }
    }
}

}  // namespace

void Gui::setThemeMode(ui::ThemeMode mode) {
    themeMode_ = mode;
    theme_ = ui::themeFor(mode, systemPrefersDark());
    core_.setTheme(theme_);
    core_.settings().setString("ui.theme", std::string(ui::themeModeName(mode)));
    applyTheme(theme_, dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0)));
    anyActivity_ = true;
}

Rgba Gui::viewportBackground() const {
    if (auto custom = ui::parseHexColor(core_.settings().getString("ui.viewportBackground", "")))
        return *custom;
    return theme_.bgViewport;
}

std::string Gui::actionLabel(const std::string& action) const {
    static const std::map<std::string, std::string> builtin = {
        {"file.new", "File > New"},
        {"file.open", "File > Open"},
        {"file.save", "File > Save"},
        {"file.saveAs", "File > Save As"},
        {"edit.undo", "Edit > Undo"},
        {"edit.redo", "Edit > Redo"},
        {"edit.selectAll", "Edit > Select All"},
        {"edit.deselect", "Edit > Deselect / cancel"},
        {"view.zoomExtents", "View > Zoom Extents"},
        {"file.exit", "File > Exit"},
        {"drafting.snapAlt", "Tools > Drafting Aids > Snap (second key)"},
        {"view.toggleCommandLine", "View > Command Line"},
        {"view.toggleProperties", "View > Object Properties"},
        {"view.expandLog", "View > Command Log"},
        {"view.textWindow", "Command history window (F2)"},
        {"view.cleanScreen", "View > Clean Screen"},
        {"view.cleanScreenAlt", "View > Clean Screen (second key)"},
        {"help.quickStart", "Help > Quick Start"},
        {"tools.settings", "Tools > Settings"}};
    if (platform::Keybindings::isCommandAction(action))
        return "Run " + action.substr(platform::Keybindings::kCommandPrefix.size());
    if (auto it = builtin.find(action); it != builtin.end())
        return it->second;
    for (const auto& item : registry_.menuItems()) {
        if (item.actionId != action)
            continue;
        std::string path = item.path;
        for (std::size_t p = path.find('/'); p != std::string::npos; p = path.find('/', p + 3))
            path.replace(p, 1, " > ");
        return path;
    }
    return action;
}

void Gui::setDpiScale(float scale) {
    dpiScale_ = std::max(0.5f, scale);
    const float total = dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0));
    buildFonts(total);
    applyTheme(theme_, total);
    anyActivity_ = true;
}

void* Gui::nativeHandle() const {
#if defined(_WIN32)
    return glfwGetWin32Window(window_);
#else
    return nullptr;
#endif
}

bool Gui::consumeActivity() {
    if (anyActivity_) {
        activityFrames_ = 30;
        anyActivity_ = false;
    }
    if (activityFrames_ > 0) {
        --activityFrames_;
        return true;
    }
    return false;
}

// ---- actions -----------------------------------------------------------------------------------

void Gui::runAction(const std::string& action) {
    if (platform::Keybindings::isCommandAction(action)) {
        core_.submitText(action.substr(platform::Keybindings::kCommandPrefix.size()));
        return;
    }
    if (action == "file.new") {
        core_.newDocument();
        startPage_ = false;
    } else if (action == "file.open")
        doOpen();
    else if (action == "file.close")
        requestAction(Pending::Close);
    else if (action == "file.exit")
        requestAction(Pending::Exit);
    else if (action == "drafting.snapAlt")
        core_.submitText("'TOGGLESNAP");
    else if (action == "view.nextDocument")
        core_.activateDocument((core_.activeDocument() + 1) % core_.documentCount());
    else if (action == "view.prevDocument")
        core_.activateDocument((core_.activeDocument() + core_.documentCount() - 1) %
                               core_.documentCount());
    else if (action == "file.save")
        doSave();
    else if (action == "file.saveAs")
        doSaveAs();
    else if (action == "edit.undo")
        history(false);
    else if (action == "edit.redo")
        history(true);
    else if (action == "edit.selectAll")
        core_.selectAll();
    else if (action == "edit.delete") {
        if (!core_.selection().empty() && !core_.runner().active())
            core_.submitText("ERASE");
    } else if (action == "edit.deselect") {
        core_.pressEscape();
        input_.clear();
    } else if (action == "view.zoomExtents")
        core_.zoomExtents();
    else if (action == "view.toggleCommandLine")
        showCommandLine_ = !showCommandLine_;
    else if (action == "view.toggleProperties")
        panelOpen_["properties"] = !panelOpen_["properties"];
    else if (action == "view.commandPalette") {
        showCommandLine_ = true;
        paletteOpen_ = true;
        suggestDismissed_ = false;
        refocusInput_ = true;
    } else if (action == "view.cursorHud") {
        cursorHud_ = !cursorHud_;
        core_.settings().setBool("hud.enabled", cursorHud_);
    } else if (action == "view.expandLog")
        logExpanded_ = !logExpanded_;
    else if (action == "view.textWindow") {
        // F2 as in AutoCAD; F2 over a row of a panel (a layer to rename) belongs to that row.
        if (!ImGui::IsAnyItemHovered())
            logExpanded_ = !logExpanded_;
    } else if (action == "view.cleanScreen" || action == "view.cleanScreenAlt")
        cleanScreen_ = !cleanScreen_;
    else if (action == "help.quickStart") {
        // F1 during a command explains that command; otherwise the quick start opens.
        const auto* prompt = core_.runner().prompt();
        if (prompt && !prompt->command.empty())
            helpCommand_ = prompt->command;
        else
            showQuickStart_ = true;
    } else if (action == "tools.settings")
        settings_.open();
    else {
        for (const auto& item : registry_.menuItems())
            if (item.actionId == action) {
                core_.submitText(item.command);
                break;
            }
    }
    anyActivity_ = true;
}

void Gui::requestAction(Pending what) {
    pending_ = what;
    continuePending();
}

// A question about unsaved work must not wait behind other windows or in the taskbar.
void Gui::bringToFront() {
    if (!window_)
        return;
    if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED))
        glfwRestoreWindow(window_);
    glfwFocusWindow(window_);
    glfwRequestWindowAttention(window_);
}

// Closing one drawing, or leaving the program: asks about each drawing with unsaved changes in
// turn (it becomes the active tab so the user sees what is being asked about).
void Gui::continuePending() {
    if (pending_ == Pending::Close) {
        if (core_.modified()) {
            openSavePrompt_ = true;
            bringToFront();
        } else
            performPending();
    } else if (pending_ == Pending::Exit) {
        for (std::size_t i = 0; i < core_.documentCount(); ++i) {
            if (!core_.documentModified(i))
                continue;
            if (!core_.activateDocument(i)) {
                pending_ = Pending::None;  // a command is still running there
                return;
            }
            openSavePrompt_ = true;
            bringToFront();
            return;
        }
        pending_ = Pending::None;
        exitRequested_ = true;
    }
}

void Gui::performPending(bool discard) {
    const Pending what = pending_;
    switch (what) {
        case Pending::Close:
            pending_ = Pending::None;
            core_.closeDocument(core_.activeDocument());
            startPage_ = true;
            break;
        case Pending::Exit:
            if (discard)
                core_.closeDocument(core_.activeDocument());
            continuePending();
            break;
        case Pending::None:
            break;
    }
}

bool Gui::requestClose() {
    if (!core_.anyDocumentModified())
        return true;
    requestAction(Pending::Exit);
    return exitRequested_;
}

// File dialogs open next to the current drawing, else in the folder of the last file picked,
// else (empty) in Documents.
std::filesystem::path Gui::dialogFolder() const {
    if (core_.hasFile())
        return core_.file().parent_path();
    // The newest recent file wins over the remembered dialog folder: it also covers files saved
    // with a typed SAVEAS path or opened from the command line, which never went through a dialog.
    if (!core_.recentFiles().empty())
        return platform::pathFromUtf8(core_.recentFiles().front()).parent_path();
    return platform::pathFromUtf8(core_.settings().getString("files.lastFolder", ""));
}

void Gui::rememberFolder(const std::filesystem::path& picked) {
    core_.settings().setString("files.lastFolder", platform::pathToUtf8(picked.parent_path()));
}

void Gui::doOpen() {
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showOpenDialog(nativeHandle(), openFilters(), "Open drawing",
                                              dialogFolder())) {
            rememberFolder(*p);
            doOpenPath(platform::pathToUtf8(*p));
        }
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::Open;
    browser_.open(FileBrowser::Mode::Open, start, "", ".tbcad;.dxf;.dwg");
}

// IMPORTDXF / EXPORTDXF ask for a file name; the dialog supplies it as the answer to that prompt.
void Gui::dxfCommandWithPath(const char* command, const std::filesystem::path& path) {
    core_.submitText(command);
    core_.submitText(platform::pathToUtf8(path));
}

void Gui::doImportDxf() {
    static const std::vector<platform::FileFilter> filters = {{"DXF drawings", "*.dxf"}};
    if (platform::nativeDialogsAvailable()) {
        if (auto p =
                platform::showOpenDialog(nativeHandle(), filters, "Import DXF", dialogFolder())) {
            rememberFolder(*p);
            dxfCommandWithPath("IMPORTDXF", *p);
        }
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::ImportDxf;
    browser_.open(FileBrowser::Mode::Open, start, "", ".dxf");
}

void Gui::doLayoutFromTemplate() {
    static const std::vector<platform::FileFilter> filters = {
        {"Drawings and templates", "*.tbcad;*.dxf"}};
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showOpenDialog(nativeHandle(), filters, "Layout from template",
                                              dialogFolder())) {
            rememberFolder(*p);
            core_.submitText("LAYOUT");
            core_.submitText("Template");
            core_.submitText(platform::pathToUtf8(*p));
        }
        return;
    }
    browserPurpose_ = BrowserPurpose::LayoutTemplate;
    browser_.open(FileBrowser::Mode::Open, core_.templateFolder(), "", ".tbcad;.dxf");
}

void Gui::doExportDxf() {
    static const std::vector<platform::FileFilter> filters = {{"DXF drawings", "*.dxf"}};
    std::filesystem::path stem = core_.hasFile() ? core_.file() : std::filesystem::path{"Drawing1"};
    stem.replace_extension(".dxf");
    std::string suggested = stem.filename().string();
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showSaveDialog(nativeHandle(), filters, "Export DXF", "dxf",
                                              suggested, dialogFolder())) {
            rememberFolder(*p);
            dxfCommandWithPath("EXPORTDXF", *p);
        }
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::ExportDxf;
    browser_.open(FileBrowser::Mode::Save, start, suggested, ".dxf");
}

void Gui::doOpenPath(const std::string& path) {
    if (core_.opening()) {
        toast("Another file is still opening.");
        return;
    }
    // A big file blocks the window for seconds: show "Opening ..." for one frame first so the
    // program does not look frozen.
    std::error_code ec;
    auto size = std::filesystem::file_size(platform::pathFromUtf8(path), ec);
    if (!ec && size > (1u << 20) && core_.openFileAsync(platform::pathFromUtf8(path))) {
        anyActivity_ = true;  // read on a worker thread; openPending() shows the progress
        return;
    }
    if (!ec && size > (8u << 20) && pendingOpen_.empty()) {
        pendingOpen_ = path;
        pendingOpenShown_ = false;
        anyActivity_ = true;
        return;
    }
    core_.setEchoOpenFailures(false);  // the message box below shows it
    auto r = core_.openFile(platform::pathFromUtf8(path));
    core_.setEchoOpenFailures(true);
    if (!r)
        lastError_ = r.error().message;
    else {
        toast("Opened " + platform::pathFromUtf8(path).filename().string());
        frontLayersIn_ = 3;  // the Levels tab must not stay in front of Layers after an open
        repairAfterOpen();
    }
    recoverAfterOpen_ = false;
}

// RECOVER <file>: once the file is open, the repair runs on it like RECOVER with no file.
void Gui::repairAfterOpen() {
    if (!recoverAfterOpen_)
        return;
    recoverAfterOpen_ = false;
    core_.submitText("RECOVER");
    core_.submitText("");
}

// Undo and redo of a big edit block the window for a while (the step is applied in one go). When
// the step is heavy, a hint is drawn first and the step runs on the next frame.
void Gui::history(bool redo) {
    if (pendingHistory_ != 0)
        return;  // a step is already waiting for its hint to be drawn
    if (core_.historyStepHeavy(redo)) {
        pendingHistory_ = redo ? 2 : 1;
        pendingHistoryLabel_ = redo ? core_.redoLabel() : core_.undoLabel();
        anyActivity_ = true;
        return;
    }
    redo ? core_.redo() : core_.undo();
}

void Gui::historyPending() {
    if (pendingHistory_ == 0)
        return;
    anyActivity_ = true;
    if (!pendingHistoryShown_) {
        pendingHistoryShown_ = true;  // draw the hint now, run the step on the next frame
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##history", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("%s ...", pendingHistoryLabel_.c_str());
            ImGui::TextDisabled("A big change is being applied.");
        }
        ImGui::End();
        return;
    }
    const bool redo = pendingHistory_ == 2;
    pendingHistory_ = 0;
    pendingHistoryShown_ = false;
    redo ? core_.redo() : core_.undo();
}

void Gui::openPending() {
    static std::string openingName;
    if (core_.xrefReloading()) {
        anyActivity_ = true;  // the reload blocks on the next frame; show why first
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##xrefreload", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("Reloading external references ...");
            ImGui::TextDisabled("A referenced file changed on disk.");
        }
        ImGui::End();
    }
    if (auto info = core_.opening()) {
        openingName = platform::pathToUtf8(info->path.filename());
        anyActivity_ = true;  // keep drawing so the elapsed time counts up
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##opening", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Opening %s ...", platform::pathToUtf8(info->path.filename()).c_str());
            const float t = static_cast<float>(ImGui::GetTime());
            ImGui::ProgressBar(-1.0f * t, ImVec2(260, 6), "");  // moving bar: still working
            ImGui::TextDisabled("%.0f s. Big drawings can take a while; the window stays usable.",
                                info->seconds);
        }
        ImGui::End();
    }
    core_.setEchoOpenFailures(false);  // the message box shows a failure
    if (auto done = core_.pollOpen()) {
        if (!*done)
            lastError_ = done->error().message;
        else {
            toast("Opened " + openingName);
            frontLayersIn_ = 3;
            repairAfterOpen();
        }
        recoverAfterOpen_ = false;
    }
    core_.setEchoOpenFailures(true);
    if (pendingOpen_.empty())
        return;
    if (!pendingOpenShown_) {
        pendingOpenShown_ = true;
        anyActivity_ = true;  // draw one more frame with the message before blocking
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##opening", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::Text(
                "Opening %s ...",
                platform::pathToUtf8(platform::pathFromUtf8(pendingOpen_).filename()).c_str());
        }
        ImGui::End();
        return;
    }
    std::string path = std::move(pendingOpen_);
    pendingOpen_.clear();
    core_.setEchoOpenFailures(false);  // the message box below shows it
    auto r = core_.openFile(platform::pathFromUtf8(path));
    core_.setEchoOpenFailures(true);
    if (!r)
        lastError_ = r.error().message;
}

void Gui::toast(const std::string& text) {
    toasts_.push_back({text, ImGui::GetTime()});
    if (toasts_.size() > 4)
        toasts_.erase(toasts_.begin());
    anyActivity_ = true;
}

void Gui::drawXrefBalloon(const ViewportRect& rect) {
    const std::vector<std::string> names = core_.xrefChangeNotice();
    if (names.empty())
        return;
    ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w - 16, rect.y + rect.h - 16), ImGuiCond_Always,
                            ImVec2(1, 1));
    if (ImGui::Begin("External reference changed", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted(names.size() == 1 ? "This external reference changed on disk:"
                                                 : "These external references changed on disk:");
        for (const std::string& n : names)
            ImGui::BulletText("%s", n.c_str());
        if (ImGui::Button(names.size() == 1 ? ("Reload " + names[0]).c_str() : "Reload them"))
            core_.reloadXrefs(names);
        ImGui::SameLine();
        if (ImGui::Button("External References..."))
            openPanel("xrefs");
        ImGui::SameLine();
        if (ImGui::Button("Dismiss"))
            core_.dismissXrefChangeNotice();
    }
    ImGui::End();
}

bool Gui::lineweights() const {
    return flag("view.lineweights", true);
}

void Gui::drawToasts(const ViewportRect& rect) {
    constexpr double kLife = 3.0, kFade = 0.5;
    const double now = ImGui::GetTime();
    std::erase_if(toasts_, [&](const Toast& t) { return now - t.shownAt > kLife; });
    if (toasts_.empty())
        return;
    anyActivity_ = true;  // keep frames coming so the toast fades out and disappears
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float y = rect.y + rect.h - 16;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const double age = now - it->shownAt;
        const float a = age > kLife - kFade ? static_cast<float>((kLife - age) / kFade) : 1.0f;
        const ImVec2 size = ImGui::CalcTextSize(it->text.c_str());
        const ImVec2 p1(rect.x + rect.w - 16, y);
        const ImVec2 p0(p1.x - size.x - 24, y - size.y - 16);
        fg->AddRectFilled(p0, p1, IM_COL32(36, 40, 48, static_cast<int>(235 * a)), 6.0f);
        fg->AddRect(p0, p1, IM_COL32(90, 100, 120, static_cast<int>(255 * a)), 6.0f);
        fg->AddText(ImVec2(p0.x + 12, p0.y + 8), IM_COL32(235, 238, 245, static_cast<int>(255 * a)),
                    it->text.c_str());
        y = p0.y - 8;
    }
}

void Gui::doSave() {
    if (!core_.hasFile() || core_.readOnlyOrigin()) {
        doSaveAs();
        return;
    }
    auto r = core_.save();
    if (!r)
        lastError_ = r.error().message;
    else
        toast("Saved " + platform::pathToUtf8(core_.file()));
}

static std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

static dxf::DxfVersion dxfChoice(int i) {
    return i == 0 ? dxf::DxfVersion::R12
                  : (i == 1 ? dxf::DxfVersion::R2000 : dxf::DxfVersion::R2007);
}

// A .dxf target first asks which DXF version to write (and what that version would lose).
void Gui::saveAsPath(const std::filesystem::path& path) {
    if (lowerExt(path) == ".dxf") {
        pendingDxfPath_ = path;
        const std::string cur = core_.settings().getString("files.dxfVersion", "auto");
        dxfVersionChoice_ = cur == "r12" ? 0 : (cur == "r2007" ? 2 : 1);
        dxfVersionWarnings_ = core_.dxfLossWarnings(dxfChoice(dxfVersionChoice_));
        openDxfVersionPrompt_ = true;
        return;
    }
    auto r = core_.saveAs(path);
    if (!r)
        lastError_ = r.error().message;
    else {
        toast("Saved " + platform::pathToUtf8(core_.file()));
        if (saveThenPending_)
            performPending(false);
    }
    saveThenPending_ = false;
}

void Gui::doSaveAs() {
    std::string ext = core_.settings().getString("files.defaultFormat", "tbcad");
    if (ext != "dxf")
        ext = "tbcad";
    if (core_.hasFile()) {
        std::string e = core_.file().extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e == ".dxf")
            ext = "dxf";
        else if (e == ".tbcad")
            ext = "tbcad";
    }
    std::string suggested = core_.hasFile() ? core_.file().filename().string() : "Drawing1." + ext;
    if (core_.readOnlyOrigin()) {  // never suggest the original (read-only) name
        ext = "tbcad";
        suggested = core_.file().stem().string() + ".tbcad";
    }
    // Very long names and folders make the shell dialog unusable, so keep both short. A drawing
    // opened read-only from a project archive starts in the last folder used for saving (or
    // Documents) instead of the huge folder the original lives in.
    constexpr std::size_t kMaxSuggested = 120;
    if (suggested.size() > kMaxSuggested)
        suggested = suggested.substr(0, kMaxSuggested - ext.size() - 1) + "." + ext;
    std::filesystem::path folder = dialogFolder();
    if (core_.readOnlyOrigin())
        folder = platform::pathFromUtf8(core_.settings().getString("files.lastFolder", ""));
    if (platform::pathToUtf8(folder).size() > 200)
        folder.clear();
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showSaveDialog(nativeHandle(), drawingFilters(ext == "dxf"),
                                              "Save drawing as", ext, suggested, folder)) {
            rememberFolder(*p);
            saveAsPath(*p);
            if (openDxfVersionPrompt_)
                return;  // the version prompt finishes the save
        }
        saveThenPending_ = false;
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::SaveAs;
    browser_.open(FileBrowser::Mode::Save, start, suggested, "." + ext);
}

// ---- shortcuts ---------------------------------------------------------------------------------

void Gui::shortcuts() {
    ImGuiIO& io = ImGui::GetIO();

    // Activity detection: keep drawing for a while after any input.
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 ||
        ImGui::IsAnyMouseDown() || io.InputQueueCharacters.Size > 0)
        anyActivity_ = true;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
            anyActivity_ = true;
            break;
        }

    const bool modal =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (modal)
        return;

    // Typing anywhere goes to the command line: keep the characters, focus the field, and hand
    // the characters back to it once it is active (the first one would be lost otherwise).
    if (!ImGui::IsAnyItemActive() && io.InputQueueCharacters.Size > 0 && !io.KeyCtrl &&
        !io.KeyAlt) {
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            char utf8[5] = {};
            ImTextCharToUtf8(utf8, io.InputQueueCharacters[i]);
            typedWhileUnfocused_ += utf8;
        }
        io.InputQueueCharacters.resize(0);
        refocusInput_ = true;
    }

    // Ctrl+V with the command line focused: its text field claims the chord (and pastes the system
    // clipboard text, usually nothing), so the drawing paste never ran. Objects copied in TBCAD
    // leave a marker as the clipboard text; an empty clipboard or the marker means "paste objects".
    if (io.WantTextInput && input_.empty() && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_V, ImGuiInputFlags_None)) {
        const char* text = ImGui::GetClipboardText();
        const bool ownText =
            !text || !*text || std::string_view(text).rfind("[TBCAD objects", 0) == 0;
        if (ownText && !core_.runner().active() && core_.registry().find("PASTECLIP")) {
            ImGui::SetKeyOwner(ImGuiKey_V, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame);
            runAction("edit.paste");
            return;
        }
    }

    // Ctrl+V while a command waits for input and the command line is not focused (a text prompt
    // such as "Output file"): paste the clipboard text into the prompt, not objects into the
    // drawing.
    if (!io.WantTextInput && core_.runner().active() && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_V, ImGuiInputFlags_None)) {
        const char* text = ImGui::GetClipboardText();
        if (text && *text && std::string_view(text).rfind("[TBCAD objects", 0) != 0) {
            std::string line(text);
            line = line.substr(0, line.find_first_of("\r\n"));
            input_ += line;
            refocusInput_ = true;
            ImGui::SetKeyOwner(ImGuiKey_V, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame);
            return;
        }
    }

    for (const auto& action : core_.keybindings().actions()) {
        auto chord = core_.keybindings().chordOf(action);
        if (!chord)
            continue;
        auto ic = imguiChordFor(*chord);
        if (!ic)
            continue;
        // While a text field has the keyboard only chords with Ctrl/Alt, Escape and F-keys work.
        // Delete erases the selection unless the command line holds text being edited.
        const bool deleteOnEmptyLine = chord->key == "DELETE" && input_.empty();
        if (io.WantTextInput && !chord->ctrl && !chord->alt && chord->key != "ESCAPE" &&
            chord->key[0] != 'F' && !deleteOnEmptyLine)
            continue;
        if (ImGui::IsKeyChordPressed(*ic)) {
            // In the focused command line Ctrl+V pastes text. The field does that itself; running
            // the drawing paste as well started PASTECLIP over a path typed at a prompt.
            if (action == "edit.paste" && io.WantTextInput) {
                const char* text = ImGui::GetClipboardText();
                const bool objects =
                    !text || !*text || std::string_view(text).rfind("[TBCAD objects", 0) == 0;
                if (!objects || core_.runner().active())
                    continue;
            }
            if (chord->key == "ESCAPE" && suggestVisible_) {
                suggestDismissed_ = true;  // Esc closes the suggestion list first
                paletteOpen_ = false;
                return;
            }
            runAction(action);
            return;
        }
    }

    // Enter and Space finish a prompt when no text field is active (AutoCAD habit).
    // Characters typed a moment ago (before the command line took focus) are still waiting in
    // typedWhileUnfocused_: Enter must send them, not an empty answer, and Space is one of them.
    if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
        const bool enter =
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        const bool space = ImGui::IsKeyPressed(ImGuiKey_Space);
        if (!typedWhileUnfocused_.empty()) {
            if (enter) {
                std::string line = input_ + typedWhileUnfocused_;
                input_.clear();
                typedWhileUnfocused_.clear();
                inputHistory_.push(line);
                core_.submitText(line);
                refocusInput_ = true;
            }
        } else if (enter || space) {
            core_.submitText("");
        }
    }
}

// ---- menus -------------------------------------------------------------------------------------

namespace {

void drawNode(const MenuNode& node, ui::AppCore& core, const platform::Keybindings& keys) {
    for (const MenuNode& child : node.children) {
        if (child.item) {
            std::string shortcut =
                child.item->actionId.empty() ? "" : keys.label(child.item->actionId);
            if (shortcut.empty()) {  // no key chord: show the command alias instead, like AutoCAD
                const std::string& line = child.item->command;
                const std::string name = line.substr(0, line.find(' '));
                if (!name.empty() && name.front() != '-' && core.registry().find(name)) {
                    auto aliases = core.registry().aliasesOf(name);
                    if (!aliases.empty())
                        shortcut = aliases.front();
                }
            }
            if (ImGui::MenuItem(child.name.c_str(), shortcut.c_str())) {
                if (child.item->command == "HATCH" || child.item->command == "GRADIENT")
                    core.openPanel("hatch");  // pattern and scale are set in the panel
                core.submitText(child.item->command);
            }
        } else if (ImGui::BeginMenu(child.name.c_str())) {
            drawNode(child, core, keys);
            ImGui::EndMenu();
        }
    }
}

}  // namespace

void Gui::registeredMenu(const std::string& top) {
    MenuNode root;
    for (const auto& item : registry_.menuItems()) {
        auto slash = item.path.find('/');
        if (item.path.substr(0, slash) != top)
            continue;
        insertItem(root, item, std::string_view(item.path).substr(slash + 1));
    }
    if (root.children.empty())
        return;
    ImGui::Separator();
    drawNode(root, core_, core_.keybindings());
}

void Gui::menuBar() {
    const auto& keys = core_.keybindings();
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", keys.label("file.new").c_str()))
            runAction("file.new");
        if (ImGui::BeginMenu("New from Template")) {
            for (const std::string& name : core_.templateNames()) {
                if (ImGui::MenuItem(name.c_str())) {
                    const std::string err = core_.newFromTemplate(name);
                    if (err.empty())
                        startPage_ = false;
                    else
                        core_.print(err);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Open...", keys.label("file.open").c_str()))
            runAction("file.open");
        if (ImGui::BeginMenu("Open Recent", !core_.recentFiles().empty())) {
            for (const auto& path : core_.recentFiles()) {
                std::string name = platform::pathToUtf8(platform::pathFromUtf8(path).filename());
                if (ImGui::MenuItem(name.c_str()))
                    doOpenPath(path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Close", keys.label("file.close").c_str()))
            runAction("file.close");
        ImGui::Separator();
        if (ImGui::MenuItem("Save", keys.label("file.save").c_str()))
            runAction("file.save");
        if (ImGui::MenuItem("Save As...", keys.label("file.saveAs").c_str()))
            runAction("file.saveAs");
        ImGui::Separator();
        if (ImGui::MenuItem("Import DXF..."))
            doImportDxf();
        if (ImGui::MenuItem("Export DXF..."))
            doExportDxf();
        registeredMenu("File");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", keys.label("file.exit").c_str()))
            requestAction(Pending::Exit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        std::string undoLabel = core_.undoLabel();
        std::string redoLabel = core_.redoLabel();
        if (ImGui::MenuItem(undoLabel.empty() ? "Undo" : undoLabel.c_str(),
                            keys.label("edit.undo").c_str(), false, !undoLabel.empty()))
            history(false);
        if (ImGui::MenuItem(redoLabel.empty() ? "Redo" : redoLabel.c_str(),
                            keys.label("edit.redo").c_str(), false, !redoLabel.empty()))
            history(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", keys.label("edit.selectAll").c_str()))
            core_.selectAll();
        if (ImGui::MenuItem("Delete", keys.label("edit.delete").c_str(), false,
                            !core_.selection().empty()))
            runAction("edit.delete");
        if (ImGui::MenuItem("Deselect", keys.label("edit.deselect").c_str(), false,
                            !core_.selection().empty()))
            core_.clearSelection();
        registeredMenu("Edit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom Extents", keys.label("view.zoomExtents").c_str()))
            core_.zoomExtents();
        if (ImGui::MenuItem("Zoom In"))
            core_.zoomAtScreen({core_.camera().viewportW / 2.0, core_.camera().viewportH / 2.0}, 1);
        if (ImGui::MenuItem("Zoom Out"))
            core_.zoomAtScreen({core_.camera().viewportW / 2.0, core_.camera().viewportH / 2.0},
                               -1);
        ImGui::Separator();
        ImGui::MenuItem("Command Line", keys.label("view.toggleCommandLine").c_str(),
                        &showCommandLine_);
        if (bool qp = core_.setting("ui.quickProps").value_or("0") == "1";
            ImGui::MenuItem("Quick Properties", nullptr, qp))
            core_.setSetting("ui.quickProps", qp ? "0" : "1");
        if (ImGui::MenuItem("Command Log", keys.label("view.expandLog").c_str(), logExpanded_))
            logExpanded_ = !logExpanded_;
        if (ImGui::MenuItem("Clean Screen", keys.label("view.cleanScreen").c_str(), cleanScreen_))
            cleanScreen_ = !cleanScreen_;
        for (const auto& p : registry_.panels())
            ImGui::MenuItem(
                p.title.c_str(),
                p.id == "properties" ? keys.label("view.toggleProperties").c_str() : nullptr,
                &panelOpen_[p.id]);
        ImGui::Separator();
        workspaceMenu();
        if (ImGui::BeginMenu("Theme")) {
            if (ImGui::MenuItem("Dark", nullptr, themeMode_ == ui::ThemeMode::Dark))
                setThemeMode(ui::ThemeMode::Dark);
            if (ImGui::MenuItem("Light", nullptr, themeMode_ == ui::ThemeMode::Light))
                setThemeMode(ui::ThemeMode::Light);
            if (ImGui::MenuItem("Follow Windows", nullptr, themeMode_ == ui::ThemeMode::System))
                setThemeMode(ui::ThemeMode::System);
            ImGui::EndMenu();
        }
        if (bool lw = lineweights(); ImGui::MenuItem("Show Line Weights", nullptr, lw))
            core_.settings().setBool("view.lineweights", !lw);
        registeredMenu("View");
        ImGui::EndMenu();
    }
    // Menus added by command modules (Draw, Modify, ...) in order of first appearance.
    std::vector<std::string> tops;
    for (const auto& item : registry_.menuItems()) {
        std::string top = item.path.substr(0, item.path.find('/'));
        if (top != "File" && top != "Edit" && top != "View" && top != "Help" &&
            std::find(tops.begin(), tops.end(), top) == tops.end())
            tops.push_back(top);
    }
    for (const auto& top : tops) {
        if (ImGui::BeginMenu(top.c_str())) {
            MenuNode root;
            for (const auto& item : registry_.menuItems()) {
                auto slash = item.path.find('/');
                if (item.path.substr(0, slash) == top)
                    insertItem(root, item, std::string_view(item.path).substr(slash + 1));
            }
            drawNode(root, core_, keys);
            ImGui::EndMenu();
        }
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Quick Start", keys.label("help.quickStart").c_str()))
            showQuickStart_ = true;
        if (ImGui::MenuItem("Command List"))
            showCommandList_ = true;
        if (ImGui::MenuItem("Report a Problem..."))
            showReport_ = true;
        if (ImGui::MenuItem("About TBCAD"))
            showAbout_ = true;
        if (ImGui::MenuItem("Third-Party Notices"))
            showNotices_ = true;
        registeredMenu("Help");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Gui::toolStrip() {
    struct Tool {
        Icon icon;
        const char* tip;
        const char* action;   // built-in action id, or nullptr
        const char* command;  // command line to run, or nullptr
    };
    struct Group {
        const char* name;
        std::vector<Tool> tools;
    };
    static const std::vector<Group> groups = {
        {"File",
         {{Icon::New, "New (Ctrl+N)", "file.new", nullptr},
          {Icon::Open, "Open (Ctrl+O)", "file.open", nullptr},
          {Icon::Save, "Save (Ctrl+S)", "file.save", nullptr}}},
        {"Edit",
         {{Icon::Undo, "Undo", "edit.undo", nullptr}, {Icon::Redo, "Redo", "edit.redo", nullptr}}},
        {"Draw",
         {{Icon::Line, "Line", nullptr, "LINE"},
          {Icon::Polyline, "Polyline", nullptr, "POLYLINE"},
          {Icon::Circle, "Circle", nullptr, "CIRCLE"},
          {Icon::Arc, "Arc", nullptr, "ARC"},
          {Icon::Rectangle, "Rectangle", nullptr, "RECTANGLE"},
          {Icon::Hatch, "Hatch", nullptr, "HATCH"},
          {Icon::Text, "Text", nullptr, "TEXT"}}},
        {"Modify",
         {{Icon::Move, "Move", nullptr, "MOVE"},
          {Icon::Copy, "Copy", nullptr, "COPY"},
          {Icon::Rotate, "Rotate", nullptr, "ROTATE"},
          {Icon::Scale, "Scale", nullptr, "SCALE"},
          {Icon::Mirror, "Mirror", nullptr, "MIRROR"},
          {Icon::Stretch, "Stretch", nullptr, "STRETCH"},
          {Icon::Trim, "Trim", nullptr, "TRIM"},
          {Icon::Offset, "Offset", nullptr, "OFFSET"},
          {Icon::Fillet, "Fillet", nullptr, "FILLET"},
          {Icon::Erase, "Erase", nullptr, "ERASE"}}},
        {"View", {{Icon::ZoomExtents, "Zoom Extents", "view.zoomExtents", nullptr}}}};

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking;
    const float iconSize = std::round(
        26.0f * dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0)));
    const float h = iconSize + ImGui::GetStyle().WindowPadding.y * 2.0f;
    if (ImGui::BeginViewportSideBar("##toolstrip", vp, ImGuiDir_Up, h, flags)) {
        bool first = true;
        // A line between groups.
        auto groupGap = [&] {
            if (!first) {
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(p.x - ImGui::GetStyle().ItemSpacing.x, p.y + 3),
                    ImVec2(p.x - ImGui::GetStyle().ItemSpacing.x, p.y + iconSize - 3),
                    ImGui::GetColorU32(ImGuiCol_Separator));
            }
            first = false;
        };
        for (const Group& group : groups) {
            const std::string key = std::string("ui.toolgroup.") + group.name;
            if (!flag(key, true))
                continue;
            bool any = false;
            for (const Tool& t : group.tools)
                any = any || t.action || core_.registry().find(t.command);
            if (!any)
                continue;
            groupGap();
            bool firstInGroup = true;
            for (const Tool& t : group.tools) {
                if (t.command && !core_.registry().find(t.command))
                    continue;
                if (!firstInGroup)
                    ImGui::SameLine(0, 2);
                firstInGroup = false;
                bool enabled = true;
                if (t.action && std::string(t.action) == "edit.undo")
                    enabled = !core_.undoLabel().empty();
                if (t.action && std::string(t.action) == "edit.redo")
                    enabled = !core_.redoLabel().empty();
                ImGui::PushID(t.tip);
                if (iconButton("##tool", t.icon, iconSize, enabled)) {
                    if (t.action)
                        runAction(t.action);
                    else {
                        if (std::string_view(t.command) == "HATCH")
                            core_.openPanel("hatch");
                        core_.submitText(t.command);
                    }
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                                         ImGuiHoveredFlags_AllowWhenDisabled)) {
                    // "Line (L) - Draw straight segments": name, alias and description come
                    // from the command registry.
                    std::string tip = t.tip;
                    if (t.command) {
                        if (cmd::Command* c = core_.registry().find(t.command)) {
                            auto aliases = core_.registry().aliasesOf(c->name());
                            if (!aliases.empty())
                                tip += " (" + aliases.front() + ")";
                            if (!c->description().empty())
                                tip += "\n" + std::string(c->description());
                        }
                    }
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
        }
        // The user's Quick Access commands (Settings > Customize, or CUI), as text buttons.
        const std::vector<std::string> quick = core_.settings().getStrings(ui::kQuickAccessKey);
        bool firstQuick = true;
        for (std::size_t i = 0; i < quick.size(); ++i) {
            const std::string label = ui::commandLabel(quick[i]);
            if (label.empty())
                continue;
            if (firstQuick)
                groupGap();
            else
                ImGui::SameLine(0, 2);
            firstQuick = false;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button((label + "##quick").c_str(), ImVec2(0, iconSize)))
                core_.submitText(quick[i]);
            ImGui::PopID();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                std::string tip = quick[i];
                if (cmd::Command* c = core_.registry().find(label); c && !c->description().empty())
                    tip += "\n" + std::string(c->description());
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        // Right-click the strip to show or hide a group, or to change the Quick Access buttons.
        if (ImGui::BeginPopupContextWindow("##toolstripMenu")) {
            for (const Group& group : groups) {
                const std::string key = std::string("ui.toolgroup.") + group.name;
                bool shown = flag(key, true);
                if (ImGui::MenuItem(group.name, nullptr, &shown))
                    core_.settings().setBool(key, shown);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Customize..."))
                core_.requestUi("settings:Customize");
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

// One quiet bar for the notices that sit above the drawing (read-only file, missing external
// references), so they cost one row instead of one row each.
void Gui::readOnlyBanner() {
    const std::string text = core_.readOnlyBanner();
    const bool showReadOnly = !text.empty() && text != dismissedReadOnly_;
    const bool showXref = core_.xrefNoticeVisible();
    if (!showXref)
        xrefListOpen_ = false;
    if (!showReadOnly && !showXref)
        return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 0.8f;
    if (ImGui::BeginViewportSideBar("##noticebar", vp, ImGuiDir_Up, h, flags)) {
        ImGui::AlignTextToFramePadding();
        if (showReadOnly) {
            ImGui::TextUnformatted(text.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Save As..."))
                runAction("file.saveAs");
        }
        if (showXref) {
            const size_t n = core_.missingXrefs().size();
            if (showReadOnly)
                ImGui::SameLine(0, 24);
            ImGui::Text("%zu external reference%s not found.", n, n == 1 ? "" : "s");
            ImGui::SameLine();
            if (ImGui::SmallButton("Show list"))
                xrefListOpen_ = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            if (showReadOnly)
                dismissedReadOnly_ = text;
            if (showXref)
                core_.dismissXrefNotice();
        }
    }
    ImGui::End();
}

// While BEDIT is open the drawing shows the block's contents: say so, and offer the way out.
void Gui::blockEditBar() {
    const std::string block = core_.setting("tmp.bedit.block").value_or("");
    if (block.empty())
        return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 0.8f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.55f, 0.32f, 0.05f, 1.0f));
    if (ImGui::BeginViewportSideBar("##beditbar", vp, ImGuiDir_Up, h, flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Editing block \"%s\"", block.c_str());
        ImGui::SameLine(0, 24);
        if (ImGui::SmallButton("Save changes"))
            core_.submitText("BSAVE");
        ImGui::SameLine();
        if (ImGui::SmallButton("Close block editor"))
            core_.submitText("BCLOSE");
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// The contextual tab: while the selection is made only of hatches, text, dimensions or blocks,
// a strip over the top of the drawing offers the tools for them (what the ribbon's contextual tab
// holds). Setting ui.contextTabs ("0" hides it).
void Gui::contextualBar() {
    if (core_.runner().active() || !flag("ui.contextTabs", true))
        return;
    const ui::ContextualTab* tab = registry_.contextualTabFor(core_.document(), core_.selection());
    if (!tab)
        return;
    // Floats over the top of the drawing (where the last frame put it), so the drawing does not
    // jump when the selection changes.
    if (lastRect_.w < 200 || lastRect_.h < 100)
        return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::SetNextWindowPos(ImVec2(lastRect_.x + 8, lastRect_.y + 6));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##contexttab", nullptr, flags)) {
        ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f), "%s", tab->title.c_str());
        ImGui::SameLine(0, 18);
        ui::PanelContext pc;
        fillPanelContext(pc);
        ImGui::PushID(tab->id.c_str());
        tab->draw(pc);
        ImGui::PopID();
        ImGui::SameLine(0, 18);
        if (ImGui::SmallButton("Close"))
            core_.clearSelection();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear the selection (Esc)");
    }
    ImGui::End();
}

float Gui::layoutTabsHeight() const {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
}

// The Model / layout tab strip under the drawing area. Model and "+" stay put; the layout tabs
// scroll sideways (mouse wheel) when there are more than fit, following the active one.
void Gui::layoutTabs(const ViewportRect& rect) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
    const float h = layoutTabsHeight();
    const std::vector<std::string> names = core_.layoutNames();
    const bool readOnly = !core_.readOnlyBanner().empty();
    std::string openPopup;
    bool templatePending = false;
    // The layout shown before this one, for "Activate previous layout".
    if (core_.activeLayout() != shownLayout_) {
        if (!shownLayout_.empty())
            previousLayout_ = shownLayout_;
        shownLayout_ = core_.activeLayout();
    }
    ImGui::SetNextWindowPos(ImVec2(rect.x, rect.y + rect.h));
    ImGui::SetNextWindowSize(ImVec2(rect.w, h));
    if (ImGui::Begin("##layouttabs", nullptr, flags)) {
        auto run = [&](std::initializer_list<std::string> lines) {
            for (const std::string& l : lines)
                core_.submitText(l);
        };
        // New / Rename / Delete need a drawing that can be changed.
        auto guarded = [&](const char* label) {
            if (ImGui::MenuItem(label, nullptr, false, !readOnly))
                return true;
            if (readOnly && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("This drawing is read-only. Use Save As first.");
            return false;
        };
        auto tab = [&](const std::string& label, const std::string& name) {
            const bool active = core_.activeLayout() == name;
            ImGui::PushID(label.c_str());
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            }
            if (ImGui::Button(label.c_str()) && !active) {
                core_.setActiveLayout(name);
            }
            if (active)
                ImGui::PopStyleColor(2);
            const auto place = std::find(names.begin(), names.end(), name);
            const std::size_t at = static_cast<std::size_t>(place - names.begin());
            if (!name.empty() && !readOnly) {
                // Drag a tab onto another to put it there; double-click renames, as in AutoCAD.
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("TBCAD_LAYOUT_TAB", name.c_str(), name.size() + 1);
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    layoutTarget_ = name;
                    openPopup = "rename";
                }
            }
            if (!name.empty() && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("TBCAD_LAYOUT_TAB")) {
                    const std::string moved(static_cast<const char*>(pl->Data));
                    if (moved != name)
                        run({"LAYOUT", "Move", moved, std::to_string(at + 1)});
                }
                ImGui::EndDragDropTarget();
            }
            if (active && name.empty())
                layoutFollowed_.clear();
            if (active && layoutFollowed_ != name) {
                layoutFollowed_ = name;
                ImGui::SetScrollHereX(0.5f);
            }
            if (ImGui::BeginPopupContextItem("##tabmenu")) {
                if (guarded("New layout..."))
                    openPopup = "new";
                if (guarded("From template..."))
                    templatePending = true;
                if (!name.empty()) {
                    if (guarded("Rename..."))
                        openPopup = "rename";
                    if (guarded("Copy"))
                        run({"LAYOUT", "Copy", name, ""});
                    if (ImGui::MenuItem("Move left", nullptr, false, !readOnly && at > 0))
                        run({"LAYOUT", "Move", name, std::to_string(at)});
                    if (ImGui::MenuItem("Move right", nullptr, false,
                                        !readOnly && at + 1 < names.size()))
                        run({"LAYOUT", "Move", name, std::to_string(at + 2)});
                    if (ImGui::MenuItem("Page setup...")) {
                        core_.setActiveLayout(name);
                        openPanel("pagesetup");
                    }
                    if (ImGui::MenuItem("Plot...")) {
                        core_.setActiveLayout(name);
                        run({"PLOTPANEL"});
                    }
                    if (guarded("Delete")) {
                        if (core_.activeLayout() == name)
                            core_.setActiveLayout("");
                        run({"LAYOUT", "Delete", name});
                    }
                    layoutTarget_ = name;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Activate Model tab", nullptr, false, core_.inPaperSpace()))
                    core_.setActiveLayout("");
                if (ImGui::MenuItem(
                        "Activate previous layout", nullptr, false,
                        !previousLayout_.empty() &&
                            std::find(names.begin(), names.end(), previousLayout_) != names.end()))
                    core_.setActiveLayout(previousLayout_);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::SameLine();
        };
        ImGui::AlignTextToFramePadding();
        tab("Model", "");
        const ImGuiStyle& st = ImGui::GetStyle();
        const float plusW = ImGui::CalcTextSize("+").x + 2 * st.FramePadding.x;
        const bool overflow = layoutScrollMax_ > 1.0f;  // measured on the previous frame
        const float arrowW = ImGui::GetFrameHeight();
        const bool paper = core_.inPaperSpace();
        const char* hint = "Sheet view: double-click a viewport to edit the model";
        const float hintW = paper ? ImGui::CalcTextSize(hint).x + 24 : 0.0f;
        float avail = ImGui::GetContentRegionAvail().x - plusW - 2 * st.ItemSpacing.x;
        if (overflow)
            avail -= 3 * (arrowW + st.ItemSpacing.x);
        // The hint gives way before the tabs do.
        const bool showHint = paper && avail - hintW > 160;
        if (showHint)
            avail -= hintW;
        if (overflow) {  // every tab stays reachable with the mouse: arrows and a full list
            if (ImGui::ArrowButton("##tabsleft", ImGuiDir_Left))
                layoutScrollDelta_ = -220.0f;
            ImGui::SameLine();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##tabscroll",
                          ImVec2(std::max(avail, 40.0f), ImGui::GetFrameHeight() + 2), false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoBackground);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
            layoutScrollDelta_ -= ImGui::GetIO().MouseWheel * 60.0f;
        if (layoutScrollDelta_ != 0.0f) {
            ImGui::SetScrollX(ImGui::GetScrollX() + layoutScrollDelta_);
            layoutScrollDelta_ = 0.0f;
        }
        layoutScrollMax_ = ImGui::GetScrollMaxX();
        for (const std::string& n : names)
            tab(n, n);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        if (overflow) {
            ImGui::SameLine();
            if (ImGui::ArrowButton("##tabsright", ImGuiDir_Right))
                layoutScrollDelta_ = 220.0f;
            ImGui::SameLine();
            if (ImGui::ArrowButton("##tabslist", ImGuiDir_Down))
                ImGui::OpenPopup("##alllayouts");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("All layouts");
            if (ImGui::BeginPopup("##alllayouts")) {
                if (ImGui::Selectable("Model", !core_.inPaperSpace()))
                    core_.setActiveLayout("");
                for (const std::string& n : names)
                    if (ImGui::Selectable(n.c_str(), core_.activeLayout() == n)) {
                        core_.setActiveLayout(n);
                        layoutFollowed_.clear();  // scroll to the chosen tab
                    }
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("+")) {
            if (readOnly)
                toast("This drawing is read-only. Use Save As first.");
            else
                openPopup = "new";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("New layout");
        if (showHint) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", hint);
        }
    }
    ImGui::End();

    if (templatePending)
        doLayoutFromTemplate();
    if (!openPopup.empty()) {
        layoutDialog_ = openPopup;
        if (openPopup == "new") {
            int n = static_cast<int>(names.size()) + 1;
            layoutNameBuf_ = "Layout" + std::to_string(n);
        } else {
            layoutNameBuf_ = layoutTarget_;
        }
        ImGui::OpenPopup("##layoutname");
    }
    if (ImGui::BeginPopup("##layoutname")) {
        ImGui::TextUnformatted(layoutDialog_ == "new" ? "New layout name" : "Rename layout");
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", layoutNameBuf_.c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputText("##n", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
        layoutNameBuf_ = buf;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        } else if ((enter || ImGui::Button("OK")) && !layoutNameBuf_.empty()) {
            if (layoutDialog_ == "new") {
                core_.submitText("LAYOUT");
                core_.submitText("New");
                core_.submitText(layoutNameBuf_);
                core_.submitText("A3");
                core_.submitText("Landscape");
                core_.setActiveLayout(layoutNameBuf_);
            } else {
                const bool wasActive = core_.activeLayout() == layoutTarget_;
                core_.submitText("LAYOUT");
                core_.submitText("Rename");
                core_.submitText(layoutTarget_);
                core_.submitText(layoutNameBuf_);
                if (wasActive)
                    core_.setActiveLayout(layoutNameBuf_);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// A drawing with external references that cannot be found looks incomplete (empty sheet
// viewports, missing backgrounds): say so, and list each name and path on request.
void Gui::xrefNotice() {
    if (!core_.xrefNoticeVisible()) {
        xrefListOpen_ = false;
        return;
    }
    const auto& missing = core_.missingXrefs();
    if (!xrefListOpen_)
        return;
    ImGui::SetNextWindowSize(ImVec2(620, 320), ImGuiCond_Appearing);
    if (ImGui::Begin("Missing external references", &xrefListOpen_)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            xrefListOpen_ = false;
        ImGui::TextDisabled(
            "Put the files at these paths (relative paths start in the drawing's folder).");
        if (ImGui::BeginTable("##missingxrefs", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Saved path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& m : missing) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(m.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(m.path.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Close"))
            xrefListOpen_ = false;
    }
    ImGui::End();
}

// Right-click on the OSNAP button: tick the snap modes (as AutoCAD's Object Snap settings).
void Gui::snapModesPopup() {
    if (ImGui::BeginPopupContextItem("##snapmodes", ImGuiPopupFlags_MouseButtonRight)) {
        const drafting::DraftingSettings cur = drafting::DraftingSettings::read(core_);
        drafting::SnapModeMask mask = cur.modes;
        bool changed = false;
        for (int i = 0; i < drafting::kSnapModeCount; ++i) {
            const auto mode = static_cast<drafting::SnapMode>(i);
            bool on = (mask & drafting::maskOf(mode)) != 0;
            const std::string label(drafting::snapModeLabel(mode));
            if (ImGui::Checkbox(label.c_str(), &on)) {
                mask = on ? (mask | drafting::maskOf(mode)) : (mask & ~drafting::maskOf(mode));
                changed = true;
            }
        }
        ImGui::Separator();
        if (ImGui::SmallButton("Select all")) {
            mask = drafting::kAllSnapModes;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all")) {
            mask = 0;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Settings..."))
            settings_.open("Drafting");
        if (changed)
            core_.setSetting(drafting::keys::osnapModes, drafting::formatSnapModes(mask));
        ImGui::EndPopup();
    }
}

void Gui::statusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    // The controls wrap onto further rows when the window is narrow (a side panel open), instead
    // of running off the right edge. The row count comes from the previous frame.
    // Even padding above and below, so the combo boxes are not cut off at the window's edge.
    constexpr float kPadY = 4.0f;
    const float h = statusRows_ * ImGui::GetFrameHeight() +
                    (statusRows_ - 1) * ImGui::GetStyle().ItemSpacing.y + 2 * kPadY;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ImGui::GetStyle().WindowPadding.x, kPadY));
    const bool statusOpen = ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, h, flags);
    ImGui::PopStyleVar();
    if (statusOpen) {
        int rows = 1;
        const ImGuiStyle& sty = ImGui::GetStyle();
        // Continue on this row when `w` more pixels fit after the last item, else start a new one.
        const auto place = [&](float w) {
            const float right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            if (right + sty.ItemSpacing.x + w <= ImGui::GetWindowWidth() - sty.WindowPadding.x) {
                ImGui::SameLine();
            } else {
                ++rows;
            }
        };
        const auto textW = [](const char* t) { return ImGui::CalcTextSize(t).x; };
        const Document& doc = core_.document();
        // Items hidden with the customization menu at the end of the bar.
        const std::string hidden = core_.setting(ui::kStatusHiddenSetting).value_or("");
        const auto shown = [&](std::string_view id) { return ui::statusItemShown(hidden, id); };
        UnitFormat fmt;
        fmt.unit = doc.header().units;
        Vec2 c = core_.cursorWorld();
        // Absurd coordinates (a stray point far away) switch to scientific notation, and the
        // readout keeps a fixed width so it never pushes the toggles or hides Y.
        auto coord = [&](double v) {
            if (std::abs(v) < 1e7)
                return formatLength(v, 3, fmt);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.3e", v);
            return std::string(buf);
        };
        ImFont* mono = monoFont();  // digits of equal width, so the readout does not jitter
        if (shown("coords")) {
            if (mono)
                ImGui::PushFont(mono);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("X %s   Y %s   %s", coord(c.x).c_str(), coord(c.y).c_str(),
                        std::string(unitSuffix(fmt.unit)).c_str());
            const float pad = ImGui::CalcTextSize("X -1000000.000   Y -1000000.000   mm").x -
                              ImGui::GetItemRectSize().x;
            if (mono)
                ImGui::PopFont();
            if (pad > 0) {
                ImGui::SameLine(0, 0);
                ImGui::Dummy({pad, 1});
            }
        } else {
            ImGui::Dummy({1, ImGui::GetFrameHeight()});  // the row starts somewhere
        }
        for (const auto& t : registry_.statusToggles()) {
            if (!shown(t.id))
                continue;
            place(textW(t.label.c_str()) + sty.FramePadding.x * 2);
            bool on = t.get();
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::SmallButton(t.label.c_str()))
                t.set(!on);
            if (on)
                ImGui::PopStyleColor();
            // Right-click on a drafting toggle opens its settings.
            if (t.id == "snap" || t.id == "grid" || t.id == "polar" || t.id == "ortho" ||
                t.id == "otrack" || t.id == "dyn") {
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Right-click for the drafting settings.");
                if (ImGui::BeginPopupContextItem(("##ds" + t.id).c_str(),
                                                 ImGuiPopupFlags_MouseButtonRight)) {
                    if (ImGui::MenuItem("Drafting settings..."))
                        settings_.open("Drafting");
                    ImGui::EndPopup();
                }
            }
            if (t.id == "selcyc" && ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Selection cycling: click the same spot again for the next object "
                    "under it.");
            if (t.id == "osnap") {
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Object snap. Right-click to choose the snap modes.");
                snapModesPopup();
            }
        }
        // A referenced file changed (XREFNOTIFY 1 or 2): a marker until it is reloaded.
        if (const std::vector<std::string> changed = core_.changedXrefs(); !changed.empty()) {
            place(textW("Xref changed") + sty.FramePadding.x * 2);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1));
            if (ImGui::SmallButton("Xref changed"))
                openPanel("xrefs");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                std::string names;
                for (const std::string& n : changed)
                    names += (names.empty() ? "" : ", ") + n;
                ImGui::SetTooltip(
                    "The file of %s changed. Click for External References; "
                    "right-click to reload.",
                    names.c_str());
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                core_.reloadXrefs({});
        }
        const Layer* layer = doc.layer(doc.header().currentLayer);
        const float layerW = ImGui::CalcTextSize("A-WALL~interior").x + 30.0f;
        if (shown("layer")) {
            place(textW("  Layer:") + sty.ItemSpacing.x + layerW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("  Layer:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(layerW);
            if (ImGui::BeginCombo("##layersw", layer ? layer->name.c_str() : "0",
                                  ImGuiComboFlags_HeightLarge)) {
                for (const auto& [id, l] : doc.layers()) {
                    const bool sel = id == doc.header().currentLayer;
                    if (ImGui::Selectable(l.name.c_str(), sel) && !sel) {
                        core_.submitText("-LAYER Set #" + std::to_string(id.value));
                        core_.submitText("");
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Layer new objects are drawn on.");
        }
        if (shown("workspace")) {
            place(130);
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo("##workspace",
                                  workspaceName_.empty() ? "Workspace" : workspaceName_.c_str(),
                                  ImGuiComboFlags_HeightLarge)) {
                for (const std::string& name : workspaces_.names())
                    if (ImGui::Selectable(name.c_str(), name == workspaceName_))
                        switchWorkspace(name);
                ImGui::Separator();
                if (ImGui::Selectable("Save current as..."))
                    core_.submitText("WSSAVE");
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Workspace: which panels are open and where (WSCURRENT)");
        }

        // Level switcher: pick the level to work on (highest first, like a section).
        if (shown("level")) {
            const Level* cur = doc.level(doc.header().currentLevel);
            const float levelW = ImGui::CalcTextSize("Level 00 (mmm)").x + 40.0f;
            place(textW("  Level:") + sty.ItemSpacing.x + levelW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("  Level:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(levelW);
            if (ImGui::BeginCombo("##levelsw", cur ? cur->name.c_str() : "?")) {
                std::vector<const Level*> order;
                for (const auto& [id, l] : doc.levels())
                    order.push_back(&l);
                std::stable_sort(order.begin(), order.end(), [](const Level* a, const Level* b) {
                    return a->elevation > b->elevation;
                });
                for (const Level* l : order) {
                    const bool sel = l->id == doc.header().currentLevel;
                    if (ImGui::Selectable(l->name.c_str(), sel) && !sel) {
                        core_.submitText("-LEVEL Set #" + std::to_string(l->id.value));
                        core_.submitText("");
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Level you work on. Ctrl+PageUp / Ctrl+PageDown steps through levels.");
        }

        // Annotation scale for new objects, and whether new objects are annotative.
        if (shown("annoscale")) {
            const double cur = currentAnnotationScale(doc.header());
            char label[48];
            std::snprintf(label, sizeof label, "1:%g", cur);
            place(textW("  Scale") + sty.ItemSpacing.x + 72 + sty.ItemSpacing.x + textW("ANNO") +
                  sty.FramePadding.x * 2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("  Scale");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(72);
            if (ImGui::BeginCombo("##annoscale", label, ImGuiComboFlags_HeightLarge)) {
                for (double v : annotationScaleList(doc.header())) {
                    char item[48];
                    std::snprintf(item, sizeof item, "1:%g", v);
                    if (ImGui::Selectable(item, v == cur)) {
                        core_.submitText("CANNOSCALE");
                        std::snprintf(item, sizeof item, "%g", v);
                        core_.submitText(item);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const bool autoOn = annotateNewObjects(doc.header());
            if (autoOn)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::SmallButton("ANNO")) {
                core_.submitText("ANNONEW");
                core_.submitText(autoOn ? "Off" : "On");
            }
            if (autoOn)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("New text, dimensions, tables and symbols follow the scale");
        }
        // Annotation visibility and automatic scale (ANNOALLVISIBLE, ANNOAUTOSCALE).
        const auto sysvarButton = [&](const char* id, const char* label, bool on,
                                      const char* sysvar, const char* tip) {
            if (!shown(id))
                return;
            place(textW(label) + sty.FramePadding.x * 2);
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::SmallButton(label) && !core_.runner().active()) {
                core_.submitText(sysvar);
                core_.submitText(on ? "0" : "1");
            }
            if (on)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tip);
        };
        sysvarButton("annovis", "VIS", annoAllVisible(doc.header()), "ANNOALLVISIBLE",
                     "Show annotative objects of every scale (on) or only those of the current "
                     "annotation scale (off)");
        sysvarButton("annoauto", "AUTO", annoAutoScale(doc.header()), "ANNOAUTOSCALE",
                     "Changing the annotation scale adds it to the annotative objects");

        // Object isolation: lit while objects are hidden or isolated; the menu isolates, hides or
        // ends it.
        if (shown("isolate")) {
            const auto it = doc.header().custom.find("isolate.hidden");
            const bool isolating = it != doc.header().custom.end() && !it->second.empty();
            place(textW("ISO") + sty.FramePadding.x * 2);
            if (isolating)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.1f, 1.0f));
            if (ImGui::SmallButton("ISO"))
                ImGui::OpenPopup("##isolate");
            if (isolating)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(isolating ? "Some objects are hidden. Click to show them again."
                                            : "Isolate or hide objects");
            if (ImGui::BeginPopup("##isolate")) {
                const bool idle = !core_.runner().active();
                if (ImGui::MenuItem("Isolate Objects", nullptr, false, idle))
                    core_.submitText("ISOLATEOBJECTS");
                if (ImGui::MenuItem("Hide Objects", nullptr, false, idle))
                    core_.submitText("HIDEOBJECTS");
                if (ImGui::MenuItem("End Object Isolation", nullptr, false, idle && isolating))
                    core_.submitText("UNISOLATEOBJECTS");
                ImGui::EndPopup();
            }
        }

        // Customization: which items the bar shows.
        {
            place(textW("...") + sty.FramePadding.x * 2);
            if (ImGui::SmallButton("...##statuscustom"))
                ImGui::OpenPopup("##statuscustom");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Choose what the status bar shows");
            if (ImGui::BeginPopup("##statuscustom")) {
                const auto item = [&](const std::string& id, const std::string& label) {
                    bool on = shown(id);
                    if (ImGui::MenuItem(label.c_str(), nullptr, &on))
                        core_.settings().setString(ui::kStatusHiddenSetting,
                                                   ui::setStatusItemShown(hidden, id, on));
                };
                item("coords", "Coordinates");
                for (const auto& t : registry_.statusToggles())
                    item(t.id, t.label);
                item("layer", "Current layer");
                item("workspace", "Workspace");
                item("level", "Level");
                item("annoscale", "Annotation scale");
                item("annovis", "Annotation visibility (VIS)");
                item("annoauto", "Add scales automatically (AUTO)");
                item("isolate", "Isolate objects (ISO)");
                item("counts", "Object counts");
                ImGui::Separator();
                if (ImGui::MenuItem("Show all"))
                    core_.settings().setString(ui::kStatusHiddenSetting, "");
                ImGui::EndPopup();
            }
        }

        // Counts on the right; on a narrow window the fps and then the object count go first,
        // so they never overlap the toggles.
        const std::string objects = std::to_string(doc.modelSpaceCount()) + " objects   ";
        const std::string selected = std::to_string(core_.selection().size()) + " selected   ";
        const std::string fps = std::to_string(static_cast<int>(fps_ + 0.5)) + " fps";
        const float free = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                           (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) -
                           2 * ImGui::GetStyle().ItemSpacing.x;
        std::string right = objects + selected + fps;
        if (ImGui::CalcTextSize(right.c_str()).x > free)
            right = objects + selected;
        if (ImGui::CalcTextSize(right.c_str()).x > free)
            right = selected;
        if (ImGui::CalcTextSize(right.c_str()).x > free)
            right = std::to_string(core_.selection().size()) + " sel";
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        if (w <= free && shown("counts")) {
            ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(right.c_str());
        }
        statusRows_ = rows;
    }
    ImGui::End();
}

// ---- windows -----------------------------------------------------------------------------------

void Gui::dockLayout(ImGuiID ds) {
    ImGuiDockNode* existing = ImGui::DockBuilderGetNode(ds);
    if (existing && (existing->IsSplitNode() || existing->Windows.Size > 0) && !rebuildDock_)
        return;  // restored from imgui.ini
    rebuildDock_ = false;
    frontLayersIn_ = 3;  // a new window takes the tab front; put Layers there once all exist
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(ds);
    ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ds, vp->WorkSize);
    ImGuiID center = ds;
    // The command area is about four rows tall (three of history and the input line), not a
    // fixed share of the window, which left it mostly empty on a tall screen.
    const float commandPx = 4.0f * ImGui::GetFrameHeightWithSpacing() + 24.0f;
    const float commandRatio = std::clamp(commandPx / std::max(vp->WorkSize.y, 1.0f), 0.07f, 0.15f);
    ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, commandRatio, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Command", bottom);
    // The last window docked is the one shown in front: Layers first, as in the plan.
    for (const auto& p : registry_.panels())
        if (p.title != "Layers")
            ImGui::DockBuilderDockWindow(p.title.c_str(), right);
    ImGui::DockBuilderDockWindow("Layers", right);
    ImGui::DockBuilderFinish(ds);
}

// A floating palette keeps at least its title bar inside the main window.
void Gui::keepOnScreen() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.CurrentWindow;
    if (!w || w->DockId != 0 || g.MovingWindow == w || (w->Flags & ImGuiWindowFlags_ChildWindow))
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float keep = 60.0f * dpiScale_;
    ImVec2 pos = w->Pos;
    pos.x =
        std::clamp(pos.x, vp->WorkPos.x + keep - w->Size.x, vp->WorkPos.x + vp->WorkSize.x - keep);
    pos.y = std::clamp(pos.y, vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - keep);
    if (pos.x != w->Pos.x || pos.y != w->Pos.y)
        ImGui::SetWindowPos(pos);
}

// A palette dragged to the left or right edge of the window docks there (the stock docking
// overlay offers no target outside the existing panels). A strip shows where it will land.
void Gui::dockEdgeDrop() {
    // ImGui forgets the moving window on the frame the button is released, so the drop is done
    // on that frame from what the last frame armed.
    static std::string armedWindow;
    static bool armedLeft = false;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* moving = g.MovingWindow;
    if (!moving && !armedWindow.empty()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float ratio = std::clamp(
            std::min(vp->WorkSize.x * 0.25f, 360.0f * dpiScale_) / std::max(vp->WorkSize.x, 1.0f),
            0.1f, 0.4f);
        ImGuiID side = ImGui::DockBuilderSplitNode(
            kDockId, armedLeft ? ImGuiDir_Left : ImGuiDir_Right, ratio, nullptr, nullptr);
        ImGui::DockBuilderDockWindow(armedWindow.c_str(), side);
        ImGui::DockBuilderFinish(kDockId);
        armedWindow.clear();
        return;
    }
    armedWindow.clear();
    if (!moving || moving->DockId != 0)
        return;
    bool isPanel = false;
    for (const auto& p : registry_.panels())
        isPanel = isPanel || p.title == moving->Name;
    if (!isPanel)
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float edge = 36.0f * dpiScale_;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool left = m.x < vp->WorkPos.x + edge;
    const bool right = m.x > vp->WorkPos.x + vp->WorkSize.x - edge;
    if (!left && !right)
        return;
    const float w = std::min(vp->WorkSize.x * 0.25f, 360.0f * dpiScale_);
    const float x0 = left ? vp->WorkPos.x : vp->WorkPos.x + vp->WorkSize.x - w;
    const ImVec2 a(x0, vp->WorkPos.y), b(x0 + w, vp->WorkPos.y + vp->WorkSize.y);
    ImGui::GetForegroundDrawList()->AddRectFilled(a, b, IM_COL32(61, 139, 253, 70));
    ImGui::GetForegroundDrawList()->AddRect(a, b, IM_COL32(61, 139, 253, 220), 0.0f, 0, 2.0f);
    armedWindow = moving->Name;
    armedLeft = left;
}

void Gui::applyWorkspace(const ui::Workspace& ws) {
    for (auto& [id, on] : panelOpen_) {
        auto it = ws.panels.find(id);
        on = it != ws.panels.end() && it->second;
    }
    showCommandLine_ = ws.commandLine;
    if (!ws.panels.count("properties"))  // workspaces saved before the panel replaced the window
        panelOpen_["properties"] = ws.properties;
    if (ws.dockIni.empty())
        rebuildDock_ = true;  // built-in workspaces use the default layout
    else
        ImGui::LoadIniSettingsFromMemory(ws.dockIni.c_str(), ws.dockIni.size());
    anyActivity_ = true;
}

void Gui::switchWorkspace(const std::string& name) {
    auto ws = workspaces_.load(name);
    if (!ws) {
        core_.print("No workspace named \"" + name + "\".");
        return;
    }
    applyWorkspace(*ws);
    workspaceName_ = ws->name;
    core_.settings().setString("workspace.current", workspaceName_);
}

void Gui::saveWorkspace(const std::string& name) {
    ui::Workspace ws;
    ws.name = name;
    for (const auto& [id, on] : panelOpen_)
        ws.panels[id] = on;
    ws.commandLine = showCommandLine_;
    ws.properties = panelOpen_["properties"];
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    ws.dockIni.assign(ini ? ini : "", ini ? size : 0);
    const std::string err = workspaces_.save(ws);
    if (!err.empty()) {
        core_.print(err);
        return;
    }
    workspaceName_ = name;
    core_.settings().setString("workspace.current", workspaceName_);
    core_.print("Workspace saved: " + name);
}

void Gui::workspaceMenu() {
    if (!ImGui::BeginMenu("Workspaces"))
        return;
    for (const std::string& name : workspaces_.names()) {
        if (ImGui::MenuItem(name.c_str(), nullptr, name == workspaceName_))
            switchWorkspace(name);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save Current Layout As...")) {
        openWorkspaceSave_ = true;
        workspaceNameInput_ = workspaceName_;
        workspaceMessage_.clear();
    }
    if (ImGui::BeginMenu("Delete")) {
        for (const std::string& name : workspaces_.names()) {
            if (workspaces_.isBuiltin(name))
                continue;
            if (ImGui::MenuItem(name.c_str())) {
                const std::string err = workspaces_.remove(name);
                if (!err.empty())
                    core_.print(err);
                else if (name == workspaceName_) {
                    workspaceName_.clear();
                    core_.settings().setString("workspace.current", "");
                }
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void Gui::workspaceDialog() {
    if (openWorkspaceSave_) {
        ImGui::OpenPopup("Save workspace");
        openWorkspaceSave_ = false;
    }
    if (!ImGui::BeginPopupModal("Save workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("Name for the current panel layout:");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(260);
    const bool enter =
        ImGui::InputText("##wsName", &workspaceNameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (!workspaceMessage_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", workspaceMessage_.c_str());
    if (ImGui::Button("Save", ImVec2(100, 0)) || enter) {
        if (workspaces_.isBuiltin(workspaceNameInput_)) {
            workspaceMessage_ = "That name is built in; choose another.";
        } else if (workspaceNameInput_.empty()) {
            workspaceMessage_ = "Give the workspace a name.";
        } else {
            saveWorkspace(workspaceNameInput_);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

namespace {

int inputCallback(ImGuiInputTextCallbackData* d) {
    auto* gui = static_cast<Gui*>(d->UserData);
    return gui->inputCallback(d);
}

}  // namespace

int Gui::inputCallback(ImGuiInputTextCallbackData* d) {
    switch (d->EventFlag) {
        case ImGuiInputTextFlags_CallbackCharFilter: {
            const auto* prompt = core_.runner().prompt();
            const bool text = prompt && prompt->kind == cmd::PromptInfo::Kind::String;
            if (d->EventChar == ' ' && input_.empty() && !text) {
                spaceAsEnter_ = true;
                return 1;  // swallow the space
            }
            return 0;
        }
        case ImGuiInputTextFlags_CallbackHistory: {
            if (suggestVisible_ && !suggestions_.empty()) {
                const int n = static_cast<int>(suggestions_.size());
                suggestNavigated_ = true;
                suggestSel_ = d->EventKey == ImGuiKey_UpArrow ? std::max(0, suggestSel_ - 1)
                                                              : std::min(n - 1, suggestSel_ + 1);
                return 0;
            }
            const std::string now(d->Buf, static_cast<std::size_t>(d->BufTextLen));
            const std::string text = d->EventKey == ImGuiKey_UpArrow ? inputHistory_.older(now)
                                                                     : inputHistory_.newer(now);
            if (text == now)
                return 0;
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, text.c_str());
            return 0;
        }
        case ImGuiInputTextFlags_CallbackCompletion: {
            if (auto* dyn = registry_.cursorFields(); dyn && core_.promptInfo() && dyn->focusNext())
                return 0;  // dynamic input: Tab goes to the first field
            if (suggestVisible_ && !suggestions_.empty()) {
                const auto& pick = suggestions_[static_cast<std::size_t>(
                    std::clamp(suggestSel_, 0, static_cast<int>(suggestions_.size()) - 1))];
                d->DeleteChars(0, d->BufTextLen);
                d->InsertChars(0, pick.name.c_str());
                paletteOpen_ = false;
                return 0;
            }
            auto matches =
                core_.complete(std::string(d->Buf, static_cast<std::size_t>(d->BufTextLen)));
            if (matches.empty())
                return 0;
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, matches[0].c_str());
            if (matches.size() > 1) {
                std::string list = "Matches:";
                for (const auto& m : matches)
                    list += " " + m;
                core_.print(list);
            }
            return 0;
        }
        default:
            return 0;
    }
}

// Dynamic input: the distance and angle (or X and Y) of the point under the cursor. Tab moves the
// keyboard through the fields; a value typed into one locks it, Enter in the last unlocked field
// places the point.
void Gui::dynamicFields(const cmd::FilterFeedback& fb) {
    cmd::CursorFieldProvider* dyn = registry_.cursorFields();
    if (!dyn)
        return;
    const std::vector<cmd::CursorField> fields = dyn->fields(fb);
    if (fields.empty()) {
        dynFocus_ = -1;
        return;
    }
    const int focus = dyn->focused();
    if (focus != dynFocus_) {
        // Tab away from a field keeps what was typed in it.
        if (dynFocus_ >= 0 && dynFocus_ < static_cast<int>(fields.size()) &&
            dynBuf_ != fields[static_cast<std::size_t>(dynFocus_)].text)
            dyn->edit(static_cast<std::size_t>(dynFocus_), dynBuf_);
        dynFocus_ = focus;
        if (focus >= 0 && focus < static_cast<int>(fields.size())) {
            dynBuf_ = fields[static_cast<std::size_t>(focus)].text;
            dynFocusPending_ = true;
        }
    }
    const Rgba snap = theme_.snap;
    const ImVec4 accent(snap.r / 255.0f, snap.g / 255.0f, snap.b / 255.0f, 1.0f);
    ImGui::PushID("dynfields");
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i)
            ImGui::SameLine();
        const cmd::CursorField& f = fields[i];
        if (static_cast<int>(i) == focus) {
            ImGui::TextUnformatted(f.label.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(72.0f * dpiScale_);
            if (dynFocusPending_) {
                ImGui::SetKeyboardFocusHere();
                dynFocusPending_ = false;
            }
            bool tab = false;
            auto callback = [](ImGuiInputTextCallbackData* d) {
                if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
                    *static_cast<bool*>(d->UserData) = true;
                return 0;
            };
            const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_AutoSelectAll |
                                              ImGuiInputTextFlags_CallbackCompletion;
            if (ImGui::InputText("##field", &dynBuf_, flags, callback, &tab)) {
                if (!dyn->edit(i, dynBuf_)) {
                    core_.print("That value cannot be used here.");
                } else if (auto p = dyn->lockedPoint(fb)) {
                    // Every field is fixed: Enter places the point.
                    ui::PointerState ps;
                    ps.screen = core_.camera().worldToScreen(*p);
                    dyn->focus(-1);
                    core_.click(ps);
                    refocusInput_ = true;
                } else {
                    dyn->focusNext();  // on to the next field
                }
            } else if (tab) {
                if (dynBuf_ != f.text)
                    dyn->edit(i, dynBuf_);
                dyn->focusNext();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  f.locked ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            const std::string shown = f.label + " " + f.text + (f.locked ? " *" : "");
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopID();
}

void Gui::logWindow() {
    if (!logExpanded_)
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.12f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.6f),
                             ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Command log", &open, ImGuiWindowFlags_NoSavedSettings)) {
        // One read-only multi-line box: the text can be selected and copied (Ctrl+C).
        const auto& lines = core_.history();
        std::string text;
        for (const auto& l : lines) {
            text += l;
            text += '\n';
        }
        if (ImGui::SmallButton("Copy all"))
            ImGui::SetClipboardText(text.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu lines. F2, Ctrl+L or Esc closes.", lines.size());
        const ImGuiID logId = ImGui::GetID("##log");
        const std::string logChild =
            std::string(ImGui::GetCurrentWindow()->Name) + "/##log_" + [&] {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%08X", logId);
                return std::string(hex);
            }();
        ImGui::InputTextMultiline("##log", &text, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        // Follow new output: the box scrolls inside a child window ImGui names after the label.
        if (lines.size() != logShownSize_) {
            if (ImGuiWindow* child = ImGui::FindWindowByName(logChild.c_str()))
                child->Scroll.y = child->ScrollMax.y + ImGui::GetTextLineHeightWithSpacing();
            logShownSize_ = lines.size();
            logFollow_ = 2;  // scroll limits are only known after a frame with the new text
        } else if (logFollow_ > 0) {
            if (ImGuiWindow* child = ImGui::FindWindowByName(logChild.c_str()))
                child->Scroll.y = child->ScrollMax.y;
            --logFollow_;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            open = false;
    }
    ImGui::End();
    if (!open)
        logExpanded_ = false;
}

void Gui::commandWindow() {
    if (!showCommandLine_)
        return;
    ImGui::SetNextWindowSize(ImVec2(700, 160), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Command", &showCommandLine_)) {
        const cmd::PromptInfo* pending = core_.promptInfo();
        // The footer (option chips and the input line) is measured on the previous frame, so
        // the history never runs under it however many rows the chips wrap into.
        const float estimate =
            ImGui::GetFrameHeightWithSpacing() + 2 +
            (pending && !pending->keywords.empty() ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
        const float footer = std::max(estimate, commandFooterH_);
        ImGui::BeginChild("##history", ImVec2(0, -footer), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const auto& lines = core_.history();
        if (ImFont* mono = monoFont())
            ImGui::PushFont(mono);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lines.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& line = lines[static_cast<std::size_t>(i)];
                const auto starts = [&](const char* w) { return line.rfind(w, 0) == 0; };
                // Errors red, warnings amber, everything else in the normal text colour.
                const bool err = starts("Unknown") || starts("Cannot") || starts("Invalid") ||
                                 starts("Error") || starts("Failed") || starts("No such") ||
                                 starts("Not found");
                const bool warn = starts("Warning");
                if (err || warn) {
                    const Rgba c = err ? theme_.danger : theme_.snap;
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                }
                ImGui::TextUnformatted(line.c_str());
                if (err || warn)
                    ImGui::PopStyleColor();
            }
        if (lines.size() != lastHistorySize_) {
            ImGui::SetScrollHereY(1.0f);
            lastHistorySize_ = lines.size();
        }
        if (monoFont())
            ImGui::PopFont();
        ImGui::EndChild();

        const float footerTop = ImGui::GetCursorPosY();
        const bool chips = optionChips();
        ImGui::AlignTextToFramePadding();
        const cmd::PromptInfo* info = core_.promptInfo();
        std::string prompt = core_.promptText();
        if (chips && info) {
            prompt = info->text;
            if (info->defaultText &&
                info->text.find("<" + *info->defaultText + ">") == std::string::npos)
                prompt += " <" + *info->defaultText + ">";
            prompt += ": ";
        }
        if (textEditorActive_)
            prompt = "Typing in the text box (Ctrl+Enter accepts, Esc cancels): ";
        ImGui::TextUnformatted(prompt.c_str());
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(-1);
        if (refocusInput_ && !textEditorActive_)
            ImGui::SetKeyboardFocusHere();
        refocusInput_ = false;
        updateSuggestions();
        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter;
        if (ImGui::InputText("##command", &input_, flags, tbcad::app::inputCallback, this)) {
            std::string line = input_;
            bool refused = false;
            // An arrow-key pick wins even over a typed word that is itself an alias ("li").
            if (suggestVisible_ && !suggestions_.empty() &&
                (suggestNavigated_ || !core_.registry().find(cmd::toUpperAscii(line)))) {
                const std::string& pick =
                    suggestions_[static_cast<std::size_t>(std::clamp(
                                     suggestSel_, 0, static_cast<int>(suggestions_.size()) - 1))]
                        .name;
                // Enter completes a typed beginning; a looser match must be picked on purpose
                // (arrow key, Tab or click), never run unasked: PLINE is not BLEND.
                if (suggestNavigated_ || pick.rfind(cmd::toUpperAscii(line), 0) == 0) {
                    line = pick;
                } else {
                    std::string hint = "Unknown command \"" + line + "\". Did you mean";
                    for (std::size_t i = 0; i < suggestions_.size() && i < 4; ++i)
                        hint += (i ? ", " : " ") + suggestions_[i].name;
                    core_.print(hint + "? Use the arrow keys and Enter to pick one.");
                    refused = true;
                }
            }
            if (refused) {
                refocusInput_ = true;
                anyActivity_ = true;
            } else {
                input_.clear();
                paletteOpen_ = false;
                inputHistory_.push(line);
                core_.submitText(line);
                refocusInput_ = true;
                anyActivity_ = true;
            }
        }
        // Esc in the focused field first leaves the field; it must still cancel the command.
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
            core_.runner().active()) {
            core_.pressEscape();
            input_.clear();
            refocusInput_ = true;
        }
        inputId_ = ImGui::GetItemID();
        const ImVec2 inputMin = ImGui::GetItemRectMin();
        const float inputW = ImGui::GetItemRectSize().x;
        if (!typedWhileUnfocused_.empty() && ImGui::IsItemActive()) {
            ImGui::GetIO().AddInputCharactersUTF8(typedWhileUnfocused_.c_str());
            typedWhileUnfocused_.clear();
        }
        if (spaceAsEnter_) {
            spaceAsEnter_ = false;
            core_.submitText("");
            refocusInput_ = true;
        }
        commandFooterH_ = ImGui::GetCursorPosY() - footerTop + 2;
        if (suggestVisible_)
            drawSuggestions(inputMin, inputW);
    }
    ImGui::End();
}

void Gui::updateSuggestions() {
    if (input_ != suggestFor_) {
        if (const auto* prompt = core_.promptInfo();
            prompt && (prompt->kind == cmd::PromptInfo::Kind::String ||
                       prompt->kind == cmd::PromptInfo::Kind::Integer))
            core_.textEdited(input_);  // TEXT / MTEXT draw what is typed
        suggestFor_ = input_;
        suggestSel_ = 0;
        suggestNavigated_ = false;
        suggestDismissed_ = false;
    }
    const bool word = input_.find_first_of(" \t") == std::string::npos;
    suggestVisible_ = core_.promptInfo() == nullptr && !suggestDismissed_ && word &&
                      (paletteOpen_ || !input_.empty());
    if (!suggestVisible_) {
        suggestions_.clear();
        return;
    }
    std::vector<std::string> recent;  // command names typed lately, latest first
    for (auto it = inputHistory_.lines().rbegin();
         it != inputHistory_.lines().rend() && recent.size() < 30; ++it) {
        std::string name = it->substr(0, it->find(' '));
        for (char& ch : name)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (const cmd::Command* c = core_.registry().find(name))
            if (std::find(recent.begin(), recent.end(), std::string(c->name())) == recent.end())
                recent.emplace_back(c->name());
    }
    suggestions_ = ui::matchCommands(core_.registry(), input_, paletteOpen_ ? 16 : 8, recent);
    if (suggestions_.empty()) {
        suggestVisible_ = false;
        return;
    }
    suggestSel_ = std::clamp(suggestSel_, 0, static_cast<int>(suggestions_.size()) - 1);
}

void Gui::drawSuggestions(ImVec2 inputPos, float width) {
    ImGui::SetNextWindowPos(ImVec2(inputPos.x, inputPos.y - 4), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(width, 760.0f), 0), ImVec2(760, 400));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##suggest", nullptr, wf)) {
        const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const auto& keys = core_.keybindings();
        // Columns: name, (alias), description; the description starts after the widest alias.
        float nameW = 0, aliasW = 0;
        for (const auto& m : suggestions_) {
            nameW = std::max(nameW, ImGui::CalcTextSize(m.name.c_str()).x);
            aliasW = std::max(aliasW, ImGui::CalcTextSize(("(" + m.alias + ")").c_str()).x);
        }
        const float aliasX = nameW + 16.0f * dpiScale_;
        const float descX = aliasX + aliasW + 16.0f * dpiScale_;
        for (std::size_t i = 0; i < suggestions_.size(); ++i) {
            const auto& m = suggestions_[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable("##row", static_cast<int>(i) == suggestSel_,
                                  ImGuiSelectableFlags_None, ImVec2(0, 0))) {
                std::string name = m.name;
                input_.clear();
                paletteOpen_ = false;
                core_.submitText(name);
                refocusInput_ = true;
                anyActivity_ = true;
            }
            if (ImGui::IsItemHovered() &&
                (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
                suggestSel_ = static_cast<int>(i);
                suggestNavigated_ = true;
            }
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::SameLine(aliasX);
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted(m.alias.empty() ? "" : ("(" + m.alias + ")").c_str());
            ImGui::SameLine(descX);
            if (m.synonym.empty()) {
                ImGui::TextUnformatted(m.description.c_str());
            } else {
                // Found through a synonym: say which word, so the name is learned.
                std::string word = m.synonym;
                for (char& ch : word)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                ImGui::Text("\"%s\": %s", word.c_str(), m.description.c_str());
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        (void)keys;
    }
    ImGui::End();
    anyActivity_ = true;
}

// Option chips for the pending prompt; each keyword is a button with its key letters underlined.
// Returns true when chips were drawn (the prompt text then omits the "[Close/Undo]" list).
bool Gui::optionChips() {
    const cmd::PromptInfo* info = core_.promptInfo();
    if (!info || info->keywords.empty())
        return false;
    for (std::size_t i = 0; i < info->keywords.size(); ++i) {
        const std::string& word = info->keywords[i];
        if (i) {
            const float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                               ImGui::CalcTextSize(word.c_str()).x +
                               2 * ImGui::GetStyle().FramePadding.x;
            if (next < ImGui::GetWindowPos().x + ImGui::GetWindowWidth() -
                           ImGui::GetStyle().WindowPadding.x)
                ImGui::SameLine();  // else the chip starts a new row
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(word.c_str())) {
            core_.submitText(word);
            refocusInput_ = true;
            anyActivity_ = true;
        }
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        const std::size_t n = ui::keywordKeyLength(info->keywords, i);
        const float w = ImGui::CalcTextSize(word.substr(0, n).c_str()).x;
        const float x0 = lo.x + ImGui::GetStyle().FramePadding.x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, hi.y - 3), ImVec2(x0 + w, hi.y - 3),
                                            ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
        ImGui::PopID();
    }
    return true;
}

// The prompt next to the cursor: what the program wants next, the typed text, and clickable
// options. Follows the cursor with a lag so the options can be reached with the mouse.
void Gui::cursorHud(const ViewportRect& rect) {
    const cmd::PromptInfo* info = core_.promptInfo();
    if (!cursorHud_ || !info) {
        hudAnchor_ = ImVec2(-1e6f, -1e6f);
        hudHovered_ = false;
        if (auto* dyn = registry_.cursorFields())
            dyn->reset();  // the point prompt is over: forget typed field values
        dynFocus_ = -1;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool inside = io.MousePos.x >= rect.x && io.MousePos.y >= rect.y &&
                        io.MousePos.x < rect.x + rect.w && io.MousePos.y < rect.y + rect.h;
    if (!inside && !hudHovered_)
        return;
    const float dx = io.MousePos.x - hudAnchor_.x;
    const float dy = io.MousePos.y - hudAnchor_.y;
    const float lag = 48.0f * dpiScale_;
    if (!hudHovered_ && (dx * dx + dy * dy > lag * lag || hudAnchor_.x < -1e5f))
        hudAnchor_ = io.MousePos;
    const bool options = !info->keywords.empty();
    // Stay inside the drawing: flip to the other side of the cursor near the right and bottom
    // edges (the size is the previous frame's).
    // Far enough from the crosshair that the box does not cover the object being pointed at.
    const float gap = 28 * dpiScale_;
    ImVec2 hudPos(hudAnchor_.x + gap, hudAnchor_.y + gap);
    if (hudPos.x + hudSize_.x > rect.x + rect.w)
        hudPos.x = hudAnchor_.x - gap - hudSize_.x;
    if (hudPos.y + hudSize_.y > rect.y + rect.h)
        hudPos.y = hudAnchor_.y - gap - hudSize_.y;
    hudPos.x = std::clamp(hudPos.x, rect.x, std::max(rect.x, rect.x + rect.w - hudSize_.x));
    hudPos.y = std::clamp(hudPos.y, rect.y, std::max(rect.y, rect.y + rect.h - hudSize_.y));
    ImGui::SetNextWindowPos(hudPos);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoMove;
    const auto* dynProvider = registry_.cursorFields();
    const bool editingField = dynProvider && dynProvider->focused() >= 0;
    if (!options && !editingField)
        wf |= ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 5));
    if (ImGui::Begin("##hud", nullptr, wf)) {
        std::string line = info->text;
        if (info->defaultText)
            line += " <" + *info->defaultText + ">";
        if (!input_.empty())
            line += ": " + input_;
        ImGui::TextUnformatted(line.c_str());
        // The snap / tracking label is drawn next to the cursor, not repeated here.
        const cmd::FilterFeedback& fb = core_.feedback();
        dynamicFields(fb);
        if (options) {
            ImGui::PushID("hudchips");
            for (std::size_t i = 0; i < info->keywords.size(); ++i) {
                if (i)
                    ImGui::SameLine();
                if (ImGui::SmallButton(info->keywords[i].c_str())) {
                    core_.submitText(info->keywords[i]);
                    refocusInput_ = true;
                }
            }
            ImGui::PopID();
        }
        hudSize_ = ImGui::GetWindowSize();
        hudHovered_ = options && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    } else {
        hudHovered_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    anyActivity_ = true;
}

// The options of the grip under the cursor (AutoCAD's multifunctional grip menu): shown after the
// cursor rests on the grip for a moment; Ctrl moves the highlight, Enter or a click runs a row.
void Gui::gripMenu(const ViewportRect& rect) {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrlEdge = io.KeyCtrl && !ctrlWasDown_;
    ctrlWasDown_ = io.KeyCtrl;
    const bool inside = io.MousePos.x >= rect.x && io.MousePos.y >= rect.y &&
                        io.MousePos.x < rect.x + rect.w && io.MousePos.y < rect.y + rect.h;
    ui::HoverMenu menu;
    const bool overGrip = (inside || gripMenuHovered_) && !io.MouseDown[0] && !io.WantTextInput &&
                          (core_.hoverMenu(menu) || gripMenuHovered_);
    if (overGrip)
        gripMenuLast_ = ImGui::GetTime();
    // Moving from the grip to the menu crosses a gap: keep the menu for a moment.
    const bool grace = gripMenuOpen_ && ImGui::GetTime() - gripMenuLast_ < 0.4;
    if (!overGrip && !grace) {
        gripMenuOpen_ = false;
        gripMenuSeen_.clear();
        return;
    }
    if (!menu.key.empty())
        gripMenu_ = menu;
    if (gripMenu_.key != gripMenuSeen_) {
        gripMenuSeen_ = gripMenu_.key;
        gripMenuSince_ = ImGui::GetTime();
        gripMenuSel_ = -1;
        gripMenuOpen_ = false;
    }
    if (!gripMenuOpen_) {
        if (ImGui::GetTime() - gripMenuSince_ < 0.35) {
            anyActivity_ = true;  // keep frames coming until the delay is over
            return;
        }
        gripMenuOpen_ = true;
    }
    const int n = static_cast<int>(gripMenu_.items.size());
    if (ctrlEdge && n > 0)
        gripMenuSel_ = (gripMenuSel_ + 1) % n;
    std::string run;
    if (gripMenuSel_ >= 0 && gripMenuSel_ < n &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        run = gripMenu_.items[static_cast<std::size_t>(gripMenuSel_)].command;
    const float s = io.DisplayFramebufferScale.x;
    const Vec2 at = core_.camera().worldToScreen(gripMenu_.anchorWorld);
    ImVec2 pos(rect.x + static_cast<float>(at.x) / s + 12.0f * dpiScale_,
               rect.y + static_cast<float>(at.y) / s + 12.0f * dpiScale_);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowBgAlpha(0.96f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoMove;
    gripMenuHovered_ = false;
    if (ImGui::Begin("##gripmenu", nullptr, wf)) {
        for (int i = 0; i < n; ++i) {
            const auto& item = gripMenu_.items[static_cast<std::size_t>(i)];
            if (ImGui::Selectable(item.label.c_str(), i == gripMenuSel_))
                run = item.command;
        }
        gripMenuHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (n > 1)
            ImGui::TextDisabled("Ctrl: next option");
    }
    ImGui::End();
    anyActivity_ = true;
    if (!run.empty()) {
        gripMenuOpen_ = false;
        gripMenuHovered_ = false;
        gripMenuSeen_.clear();
        core_.submitPanelText(run);
    }
}

void Gui::docTabs() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    const float h = ImGui::GetFrameHeight() + 4.0f * dpiScale_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 0));
    if (ImGui::BeginViewportSideBar("##doctabs", vp, ImGuiDir_Up, h, flags)) {
        if (ImGui::BeginTabBar("##docs", ImGuiTabBarFlags_FittingPolicyScroll |
                                             ImGuiTabBarFlags_TabListPopupButton)) {
            const std::size_t n = core_.documentCount();
            const std::size_t active = core_.activeDocument();
            const bool external = active != shownActive_;
            std::optional<std::size_t> toActivate, toClose;
            for (std::size_t i = 0; i < n; ++i) {
                std::string label =
                    core_.documentTitle(i) + "###doc" + std::to_string(core_.documentId(i));
                ImGuiTabItemFlags tf = ImGuiTabItemFlags_None;
                if (core_.documentModified(i))
                    tf |= ImGuiTabItemFlags_UnsavedDocument;
                if (external && i == active)
                    tf |= ImGuiTabItemFlags_SetSelected;
                bool open = true;
                if (ImGui::BeginTabItem(label.c_str(), &open, tf)) {
                    if (!external && i != active)
                        toActivate = i;
                    ImGui::EndTabItem();
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                    open = false;  // middle-click closes a tab
                if (ImGui::BeginPopupContextItem("##doctabmenu")) {
                    // The commands work on the active drawing, so the tab is made active first.
                    const bool here = i == active || (!external && core_.activateDocument(i));
                    if (ImGui::MenuItem("Save", nullptr, false, here))
                        runAction("file.save");
                    if (ImGui::MenuItem("Close", nullptr, false, here))
                        open = false;
                    if (ImGui::MenuItem("Copy full path", nullptr, false, here && core_.hasFile()))
                        ImGui::SetClipboardText(core_.file().string().c_str());
                    ImGui::EndPopup();
                }
                if (!open)
                    toClose = i;
            }
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                runAction("file.new");
            ImGui::EndTabBar();
            shownActive_ = active;
            if (toActivate && !core_.activateDocument(*toActivate))
                shownActive_ = static_cast<std::size_t>(-1);  // put the selection back
            if (toClose) {
                if (*toClose == core_.activeDocument() || core_.activateDocument(*toClose))
                    requestAction(Pending::Close);
                else
                    shownActive_ = static_cast<std::size_t>(-1);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// Shown over the empty viewport while only an untouched "Untitled" drawing is open.
void Gui::startPage(const ViewportRect& rect) {
    // The card steps aside as soon as a command starts. It hangs from a fixed top edge, so the
    // buttons stay put however long the recent list is.
    if (!startPage_ || !core_.isPristine() || core_.documentCount() != 1 || core_.runner().active())
        return;
    ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w * 0.5f, rect.y + std::min(60.0f, rect.h * 0.1f)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    if (ImGui::Begin("##start", nullptr, wf)) {
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("TBCAD");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Free 2D CAD");
        ImGui::Spacing();
        if (ImGui::Button("New drawing", ImVec2(150, 0)))
            runAction("file.new");
        ImGui::SameLine();
        if (ImGui::Button("Open...", ImVec2(110, 0)))
            runAction("file.open");
        ImGui::SameLine();
        if (ImGui::Button("Import DXF", ImVec2(110, 0)))
            doImportDxf();

        const std::vector<std::string> templates = core_.templateNames();
        if (!templates.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Start from a template");
            int shown = 0;
            for (const std::string& name : templates) {
                if (++shown > 6)
                    break;
                if (shown > 1 && (shown - 1) % 3 != 0)
                    ImGui::SameLine();
                if (ImGui::Button((name + "##tmpl").c_str(), ImVec2(130, 0))) {
                    const std::string err = core_.newFromTemplate(name);
                    if (err.empty())
                        startPage_ = false;
                    else
                        lastError_ = err;
                }
            }
        }

        // Recent drawings as cards: the picture taken when the drawing was last opened or saved,
        // and its name. Four to a row, two rows.
        const auto recent = core_.recentFiles();
        if (!recent.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Recent");
            const ImVec2 thumb(160 * dpiScale_, 110 * dpiScale_);
            int shown = 0;
            for (const auto& path : recent) {
                if (shown == 8)
                    break;
                if (shown % 4 != 0)
                    ImGui::SameLine();
                ++shown;
                const std::filesystem::path fp = platform::pathFromUtf8(path);
                const std::string name = platform::pathToUtf8(fp.filename());
                ImGui::PushID(path.c_str());
                ImGui::BeginGroup();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const bool clicked = ImGui::InvisibleButton(name.c_str(), thumb);
                const bool hovered = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 end(at.x + thumb.x, at.y + thumb.y);
                const unsigned tex =
                    thumbnailLoader_ ? thumbnailLoader_(core_.thumbnailFile(path)) : 0u;
                if (tex) {
                    dl->AddImage(static_cast<ImTextureID>(tex), at, end);
                } else {
                    dl->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
                    std::string ext = platform::pathToUtf8(fp.extension());
                    if (!ext.empty() && ext[0] == '.')
                        ext.erase(0, 1);
                    for (char& c : ext)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    const ImVec2 ts = ImGui::CalcTextSize(ext.c_str());
                    dl->AddText(
                        ImVec2(at.x + (thumb.x - ts.x) * 0.5f, at.y + (thumb.y - ts.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), ext.c_str());
                }
                dl->AddRect(at, end,
                            ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Border),
                            0.0f, 0, hovered ? 2.0f : 1.0f);
                // The name, cut to the card's width.
                std::string label = name, base = name;
                while (!base.empty() && ImGui::CalcTextSize(label.c_str()).x > thumb.x) {
                    // One whole UTF-8 character off the end.
                    while (!base.empty() &&
                           (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80)
                        base.pop_back();
                    if (!base.empty())
                        base.pop_back();
                    label = base + "...";
                }
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndGroup();
                if (hovered)
                    ImGui::SetTooltip("%s", path.c_str());
                if (clicked)
                    doOpenPath(path);
                ImGui::PopID();
            }
        }
        if (flag("ui.gettingAround", true)) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Getting around");
            ImGui::BulletText("Type a command or its short name, then Enter.");
            ImGui::BulletText("Enter repeats the last command, Esc cancels.");
            ImGui::BulletText("Middle-drag pans, the wheel zooms.");
            ImGui::BulletText("Ctrl+Shift+P lists every command.");
            bool hide = false;
            if (ImGui::Checkbox("Don't show again", &hide) && hide)
                core_.settings().setBool("ui.gettingAround", false);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

namespace {

// Newlines typed in the box become the \P the text commands expect, and back.
std::string newlinesToCodes(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r')
            continue;
        if (text[i] == '\n')
            out += "\\P";
        else
            out += text[i];
    }
    return out;
}

std::string codesToNewlines(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'P') {
            out += '\n';
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

}  // namespace

// TEXT, MTEXT and TEXTEDIT: type the text in a box where it will go. The command line keeps
// working; what is typed in either place shows in the drawing as it is typed.
void Gui::textEditor(const ViewportRect& rect) {
    const auto* prompt = core_.promptInfo();
    if (!prompt || prompt->kind != cmd::PromptInfo::Kind::String || !prompt->editor) {
        textEditorActive_ = false;
        return;
    }
    const cmd::TextEditorHint& hint = *prompt->editor;
    if (!textEditorActive_) {
        textEditorActive_ = true;
        textEditorFocus_ = 2;
        textEditorBuf_ = codesToNewlines(prompt->defaultText.value_or(""));
        textEditorSent_ = textEditorBuf_;
        if (!textEditorBuf_.empty())
            core_.textEdited(newlinesToCodes(textEditorBuf_));
    }
    const float s = ImGui::GetIO().DisplayFramebufferScale.x;
    const Vec2 top = core_.camera().worldToScreen(hint.anchor);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    int lines = 1;
    for (char c : textEditorBuf_)
        lines += c == '\n';
    // The box grows with the text: at least the rectangle drawn, at least one line.
    const float maxW = std::max(120.0f, rect.w - 24.0f);
    const float maxH = std::max(80.0f, rect.h - 64.0f * dpiScale_);
    const float boxW =
        std::min(maxW, std::max(240.0f * dpiScale_,
                                static_cast<float>(hint.widthMm * core_.camera().pixelsPerMm) / s));
    const float drawnH = static_cast<float>(hint.heightMm * core_.camera().pixelsPerMm) / s;
    const float boxH =
        std::min(maxH, std::max({drawnH, lineH * static_cast<float>(lines) + 10.0f, lineH * 2}));
    ImVec2 pos(rect.x + static_cast<float>(top.x) / s, rect.y + static_cast<float>(top.y) / s);
    pos.x = std::clamp(pos.x, rect.x, std::max(rect.x, rect.x + rect.w - boxW));
    pos.y = std::clamp(pos.y, rect.y, std::max(rect.y, rect.y + rect.h - boxH - 32.0f * dpiScale_));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    bool accept = false;
    // The window is hidden for its first frame, and a new focus request every frame would
    // restart the search: ask once, on the second frame.
    auto focusEditor = [&] {
        if (textEditorFocus_ > 0 && --textEditorFocus_ == 0)
            ImGui::ActivateItemByID(ImGui::GetItemID());
    };
    if (ImGui::Begin("##textedit", nullptr, wf)) {
        const ImGuiInputTextFlags flags = hint.multiline ? ImGuiInputTextFlags_AllowTabInput
                                                         : ImGuiInputTextFlags_EnterReturnsTrue;
        if (hint.multiline && prompt->command == "MTEXT") {
            // Text height, justification and style, applied to the whole MTEXT as you type.
            // tmp.* keys live in the command host's transient store, not in the saved settings.
            auto getS = [&](const char* k, const char* d) { return core_.setting(k).value_or(d); };
            bool changed = false;
            double h = std::atof(getS("tmp.mtext.height", "2.5").c_str());
            ImGui::SetNextItemWidth(72 * dpiScale_);
            if (ImGui::InputDouble("Height", &h, 0, 0, "%.4g") && h > 0) {
                core_.setSetting("tmp.mtext.height", std::to_string(h));
                changed = true;
            }
            ImGui::SameLine();
            static const char* const kJust[] = {"TL", "TC", "TR", "ML", "MC",
                                                "MR", "BL", "BC", "BR"};
            static const char* const kJustLong[] = {"Top left",    "Top center",    "Top right",
                                                    "Middle left", "Middle center", "Middle right",
                                                    "Bottom left", "Bottom center", "Bottom right"};
            const std::string cur = getS("tmp.mtext.attach", "TL");
            const char* curLong = "Top left";
            for (int i = 0; i < 9; ++i)
                if (cur == kJust[i])
                    curLong = kJustLong[i];
            ImGui::SetNextItemWidth(120 * dpiScale_);
            if (ImGui::BeginCombo("Justify", curLong)) {
                for (int i = 0; i < 9; ++i)
                    if (ImGui::Selectable(kJustLong[i], cur == kJust[i])) {
                        core_.setSetting("tmp.mtext.attach", kJust[i]);
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const std::string styleName = getS("tmp.mtext.style", "");
            ImGui::SetNextItemWidth(130 * dpiScale_);
            if (ImGui::BeginCombo("Style", styleName.c_str())) {
                for (const auto& [id, ts] : core_.document().textStyles())
                    if (ImGui::Selectable(ts.name.c_str(), ts.name == styleName)) {
                        core_.setSetting("tmp.mtext.style", ts.name);
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            // Columns: none, dynamic (balanced) or static (each as tall as the rectangle or the
            // height typed), cut from the rectangle's width.
            const int cols = std::max(1, std::atoi(getS("tmp.mtext.columns", "1").c_str()));
            const bool stat = getS("tmp.mtext.colkind", "dynamic") == "static";
            const std::string colLabel =
                cols <= 1 ? std::string("No columns")
                          : std::to_string(cols) + (stat ? " static" : " dynamic");
            ImGui::SetNextItemWidth(110 * dpiScale_);
            if (ImGui::BeginCombo("Columns", colLabel.c_str())) {
                if (ImGui::Selectable("No columns", cols <= 1)) {
                    core_.setSetting("tmp.mtext.columns", "1");
                    changed = true;
                }
                for (const char* kind : {"dynamic", "static"})
                    for (int n = 2; n <= 4; ++n) {
                        const std::string label = std::to_string(n) + " " + kind;
                        if (ImGui::Selectable(label.c_str(), label == colLabel)) {
                            core_.setSetting("tmp.mtext.columns", std::to_string(n));
                            core_.setSetting("tmp.mtext.colkind", kind);
                            changed = true;
                        }
                    }
                ImGui::EndCombo();
            }
            if (cols > 1 && stat) {
                ImGui::SameLine();
                double ch = std::atof(getS("tmp.mtext.colheight", "0").c_str());
                ImGui::SetNextItemWidth(64 * dpiScale_);
                if (ImGui::InputDouble("Column height", &ch, 0, 0, "%.4g") && ch >= 0) {
                    core_.setSetting("tmp.mtext.colheight", std::to_string(ch));
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = the height of the rectangle");
            }
            ImGui::SameLine();
            bool mask = getS("tmp.mtext.bgmask", "0") == "1";
            if (ImGui::Checkbox("Mask", &mask)) {
                core_.setSetting("tmp.mtext.bgmask", mask ? "1" : "0");
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Background mask: hide what is behind the text");
            if (changed)
                core_.textEdited(newlinesToCodes(textEditorBuf_));
        }
        if (hint.multiline) {
            const double barHeight =
                std::atof(core_.setting("tmp.mtext.height").value_or("2.5").c_str());
            formatBar_.draw(core_.document(), textEditorBuf_, barHeight > 0 ? barHeight : 2.5);
            ImGui::InputTextMultiline("##text", &textEditorBuf_, ImVec2(boxW, boxH),
                                      flags | ImGuiInputTextFlags_CallbackAlways,
                                      &textcmd::FormatBar::callback, &formatBar_);
            focusEditor();
            // A bar button took the focus: give it back so the box applies the edit, with the
            // selection it had.
            if (formatBar_.pending() && !ImGui::IsItemActive())
                ImGui::ActivateItemByID(ImGui::GetItemID());
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter))
                accept = true;
        } else {
            ImGui::SetNextItemWidth(boxW);
            accept = ImGui::InputText("##text", &textEditorBuf_, flags);
            focusEditor();
        }
        if (ImGui::SmallButton("OK"))
            accept = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            core_.pressEscape();
        ImGui::SameLine();
        ImGui::TextDisabled(hint.multiline ? "Ctrl+Enter accepts, Esc cancels"
                                           : "Enter accepts, Esc cancels");
    }
    ImGui::End();
    if (textEditorBuf_ != textEditorSent_) {
        textEditorSent_ = textEditorBuf_;
        core_.textEdited(newlinesToCodes(textEditorBuf_));
    }
    if (accept) {
        textEditorActive_ = false;
        core_.submitText(newlinesToCodes(textEditorBuf_));
        refocusInput_ = true;
        anyActivity_ = true;
    }
}

void Gui::quickProperties(const ViewportRect& rect) {
    if (core_.setting("ui.quickProps").value_or("0") != "1" || core_.selection().empty() ||
        core_.runner().active() || !registry_.quickProperties())
        return;
    // QPLOCATION: next to the pointer where the selection was made (AutoCAD's default), or in the
    // top right corner of the drawing ("ui.quickProps.location" = "corner").
    if (core_.setting("ui.quickProps.location").value_or("cursor") == "corner") {
        ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w - 12, rect.y + 12), ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));
    } else {
        std::size_t key = core_.selection().size();
        for (EntityId id : core_.selection())
            key = (key ^ id.value) * 1099511628211ull;
        if (key != quickPropsKey_ || !quickPropsPlaced_) {
            quickPropsKey_ = key;
            quickPropsPlaced_ = true;
            // Selected without the pointer over the window (a command, select all from a key):
            // the middle of the drawing.
            quickPropsAt_ = ImGui::IsMousePosValid()
                                ? ImGui::GetIO().MousePos
                                : ImVec2(rect.x + rect.w / 2, rect.y + rect.h / 2);
        }
        // Below and to the right of the pointer, kept inside the drawing area; it never covers
        // the spot that was clicked.
        // The box's size in the last frame (it sizes itself to its contents).
        const float kW = quickPropsSize_.x, kH = quickPropsSize_.y, kGap = 24;
        float x = quickPropsAt_.x + kGap, y = quickPropsAt_.y + kGap;
        if (x + kW > rect.x + rect.w)
            x = quickPropsAt_.x - kGap - kW;
        if (y + kH > rect.y + rect.h)
            y = quickPropsAt_.y - kGap - kH;
        x = std::clamp(x, rect.x + 4, std::max(rect.x + 4, rect.x + rect.w - kW - 4));
        y = std::clamp(y, rect.y + 4, std::max(rect.y + 4, rect.y + rect.h - kH - 4));
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(260, 0), ImVec2(360, 520));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##quickprops", nullptr, wf)) {
        quickPropsSize_ = ImGui::GetWindowSize();
        ui::PanelContext pc;
        fillPanelContext(pc);
        registry_.quickProperties()(pc);
    }
    ImGui::End();
}

void Gui::fillPanelContext(ui::PanelContext& pc) {
    pc.doc = &core_.document();
    pc.host = &core_;
    pc.selection = &core_.selection();
    pc.camera = &core_.camera();
    pc.runCommand = [this](const std::string& line) { core_.submitPanelText(line); };
    pc.cancelCommand = [this] {
        if (core_.runner().active())
            core_.pressEscape();
    };
    pc.commandActive = core_.runner().active();
    pc.selectionRevision = core_.selectionRevision();
    pc.drawPropertySections = [this, &pc](EntityId id) {
        const Entity* e = core_.document().find(id);
        if (!e)
            return;
        pc.multiSelection = false;
        for (const auto* section : registry_.sectionsFor(kind(*e))) {
            ImGui::Separator();
            section->draw(pc, id);
        }
    };
    pc.drawMultiSections = [this, &pc]() {
        // One call per kind in the selection, each with the first object of that kind. Finding
        // them walks the whole selection, so the result is kept until the selection or the
        // drawing changes (select-all on a big drawing would otherwise walk it every frame).
        static std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> cachedKey{};
        static std::vector<EntityId> firsts;
        const std::vector<EntityId>& sel = *pc.selection;
        const std::tuple key{core_.document().uid(), core_.document().revision(),
                             core_.selectionRevision()};
        if (key != cachedKey) {
            cachedKey = key;
            firsts.clear();
            std::set<EntityKind> seen;
            for (EntityId id : sel) {
                const Entity* e = core_.document().find(id);
                if (e && seen.insert(kind(*e)).second)
                    firsts.push_back(id);
            }
        }
        pc.multiSelection = true;
        for (EntityId id : firsts) {
            const Entity* e = core_.document().find(id);
            if (!e)
                continue;
            for (const auto* section : registry_.sectionsFor(kind(*e)))
                section->draw(pc, id);
        }
        pc.multiSelection = false;
    };
}

void Gui::panels() {
    ui::PanelContext pc;
    fillPanelContext(pc);
    for (const std::string& id : core_.takePanelCloses())
        panelOpen_[id] = false;
    for (const std::string& id : core_.takePanelRequests()) {
        panelOpen_[id] = true;
        for (const auto& panel : registry_.panels())
            if (panel.id == id)
                focusPanel_ = panel.title;  // bring its tab to the front
    }
    // A panel the saved layout has never seen (imgui.ini from an older version, or a panel added
    // later) is docked beside the Object Properties panel instead of floating over the tabs.
    ImGuiID sideDock = 0;
    for (const char* name : {"Object Properties", "Properties", "Layers", "Levels"}) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name); w && w->DockId != 0)
            sideDock = w->DockId;
        else if (ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(ImHashStr(name)))
            sideDock = ws->DockId;
        if (sideDock != 0)
            break;
    }
    if (sideDock != 0) {
        // Never tab the side panels into the command window's dock.
        ImGuiID commandDock = 0;
        if (ImGuiWindow* w = ImGui::FindWindowByName("Command"); w && w->DockId != 0)
            commandDock = w->DockId;
        else if (ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(ImHashStr("Command")))
            commandDock = ws->DockId;
        if (commandDock == sideDock)
            sideDock = 0;
    }
    if (sideDock == 0 && !dockFallbackTried_) {
        // Nothing is docked at the side (a layout file from a much older version): start from the
        // default layout once rather than leave panels floating over the document tabs.
        dockFallbackTried_ = true;
        rebuildDock_ = true;
    }
    dockEdgeDrop();
    for (const auto& p : registry_.panels()) {
        bool& open = panelOpen_[p.id];
        if (!open)
            continue;
        if (sideDock != 0) {
            ImGui::SetNextWindowDockID(sideDock, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
        }
        const bool shown = ImGui::Begin(p.title.c_str(), &open);
        keepOnScreen();
        if (shown)
            p.draw(pc);
        ImGui::End();
        // Layer Walk puts the layers back when its window is closed (unless told to keep them).
        if (!open && p.id == "laywalk" && core_.setting("laywalk.restore").value_or("1") != "0" &&
            !core_.setting("tmp.laywalk.state").value_or("").empty())
            core_.submitText("-LAYWALK Restore");
    }
}

// ---- viewport ----------------------------------------------------------------------------------

void Gui::viewportInput(const ViewportRect& rect) {
    ImGuiIO& io = ImGui::GetIO();
    const double s = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0;
    const bool mouseValid = io.MousePos.x > -1e6f;
    const Vec2 mouse{(io.MousePos.x - rect.x) * s, (io.MousePos.y - rect.y) * s};
    const bool inside = mouseValid && io.MousePos.x >= rect.x && io.MousePos.y >= rect.y &&
                        io.MousePos.x < rect.x + rect.w && io.MousePos.y < rect.y + rect.h;
    const bool hovered = inside && !io.WantCaptureMouse;
    const double dt = std::max(1e-4, static_cast<double>(io.DeltaTime));
    const ui::PointerState ps{mouse, io.KeyShift, io.KeyCtrl};

    // The picture follows the state by one frame (the status bar, the overlay and the scene are
    // built from what the previous frame left), so keep drawing for a few frames after the
    // pointer moves, a button changes, text is typed or the drawing, selection or log changes.
    // Otherwise the last change would wait for the next event to appear.
    {
        const std::size_t stamp = core_.history().size() * 31 + core_.selection().size() * 7 +
                                  std::hash<std::string>{}(core_.undoLabel());
        if (stamp != activityStamp_ || io.MouseDelta.x != 0 || io.MouseDelta.y != 0 ||
            io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2] ||
            io.InputQueueCharacters.Size > 0)
            anyActivity_ = true;
        activityStamp_ = stamp;
    }

    // The press that made a tile active does nothing else.
    if (tileJustActivated_) {
        tileJustActivated_ = false;
        pendingNotches_ = 0;
        return;
    }

    // Middle button: pan; double click: zoom extents.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        panning_ = true;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
        core_.zoomExtents();
        panning_ = false;
    }
    if (panning_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            core_.panPixels(io.MouseDelta.x * s, io.MouseDelta.y * s);
        else
            panning_ = false;
    }

    // Wheel: 1.2 per notch about the cursor, eased over a few frames.
    if (hovered && io.MouseWheel != 0) {
        pendingNotches_ += io.MouseWheel;
        zoomAnchor_ = mouse;
    }
    if (std::fabs(pendingNotches_) > 1e-3) {
        double step = pendingNotches_ * (1.0 - std::exp(-22.0 * dt));
        if (std::fabs(pendingNotches_ - step) < 0.02)
            step = pendingNotches_;
        core_.zoomAtScreen(zoomAnchor_, step);
        pendingNotches_ -= step;
        anyActivity_ = true;
    }

    // Real-time zoom or pan (ZOOM <Enter>, PAN, the navigation bar): a left drag zooms (up = in)
    // or pans; Esc, Enter or a right click ends it.
    if (realtime_ != Realtime::Off) {
        anyActivity_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
            (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            realtime_ = Realtime::Off;
            realtimeDrag_ = false;
            return;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            realtimeDrag_ = true;
        if (realtimeDrag_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            realtimeDrag_ = false;
        if (realtimeDrag_ && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            if (realtime_ == Realtime::Pan)
                core_.panPixels(io.MouseDelta.x * s, io.MouseDelta.y * s);
            else
                core_.zoomAtScreen({core_.camera().viewportW / 2.0, core_.camera().viewportH / 2.0},
                                   -io.MouseDelta.y / 40.0);
        }
        if (hovered) {
            core_.pointerMoved(ps);
            ImGui::SetMouseCursor(realtime_ == Realtime::Pan ? ImGuiMouseCursor_Hand
                                                             : ImGuiMouseCursor_ResizeNS);
            const char* tip = realtime_ == Realtime::Pan
                                  ? "Real-time pan: drag. Esc or Enter ends it."
                                  : "Real-time zoom: drag up to zoom in, down to zoom out. Esc "
                                    "or Enter ends it.";
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 at(io.MousePos.x + 18, io.MousePos.y + 18);
            const ImVec2 sz = ImGui::CalcTextSize(tip);
            fg->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + sz.x + 4, at.y + sz.y + 2),
                              IM_COL32(24, 26, 30, 220), 3.0f);
            fg->AddText(at, IM_COL32(230, 232, 235, 255), tip);
        }
        return;
    }

    // Left button: click, or drag a selection rectangle.
    const auto* prompt = core_.runner().prompt();
    const bool pointPrompt = prompt && (prompt->kind == cmd::PromptInfo::Kind::Point ||
                                        prompt->kind == cmd::PromptInfo::Kind::Distance ||
                                        prompt->kind == cmd::PromptInfo::Kind::Angle);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        leftDown_ = true;
        dragging_ = false;
        doubleClick_ = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        pressPos_ = mouse;
        // A point is taken when the button goes down, so the rubber band follows the pointer
        // while it is dragged; releasing after a drag answers the next point prompt.
        pressTookPoint_ = pointPrompt && !io.KeyAlt;
        if (pressTookPoint_)
            core_.click(ps);
        else if (!prompt && !io.KeyAlt && !doubleClick_)
            pressTookPoint_ = core_.pressOnGrip(ps);  // a grip starts its edit on press
    }
    if (leftDown_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!pointPrompt && !dragging_ && !pressTookPoint_ &&
                std::hypot(mouse.x - pressPos_.x, mouse.y - pressPos_.y) > 4.0 * s)
                dragging_ = true;
            if (dragging_ && io.KeyAlt) {
                // Alt+drag: freeform lasso instead of a rectangle.
                if (lassoPath_.empty())
                    lassoPath_.push_back(pressPos_);
                if (std::hypot(mouse.x - lassoPath_.back().x, mouse.y - lassoPath_.back().y) >
                    3.0 * s)
                    lassoPath_.push_back(mouse);
                core_.setLassoPath(lassoPath_);
                core_.setSelectionDrag(std::nullopt);
            } else if (dragging_) {
                lassoPath_.clear();
                core_.setLassoPath({});
                core_.setSelectionDrag(std::make_pair(pressPos_, mouse));
            }
        } else if (pressTookPoint_) {
            // Press-drag-release: the release is the next point when the pointer travelled.
            const auto* next = core_.runner().prompt();
            const bool nextIsPoint = next && (next->kind == cmd::PromptInfo::Kind::Point ||
                                              next->kind == cmd::PromptInfo::Kind::Distance ||
                                              next->kind == cmd::PromptInfo::Kind::Angle);
            if (nextIsPoint && std::hypot(mouse.x - pressPos_.x, mouse.y - pressPos_.y) > 4.0 * s)
                core_.click(ps);
            pressTookPoint_ = leftDown_ = dragging_ = false;
        } else {
            // SELCYC switches cycling on and off; SELECTIONCYCLING 0 also turns it off.
            const bool cyclingOn = flag("ui.selectionCycling", false) &&
                                   core_.setting("selection.cycling").value_or("2") != "0";
            if (dragging_ && !lassoPath_.empty()) {
                core_.lassoSelect(lassoPath_, ps);
            } else if (dragging_) {
                core_.dragSelect(pressPos_, mouse, ps);
            } else if (!io.KeyAlt && cyclingOn && !doubleClick_ &&
                       core_.setting("selection.cycling").value_or("2") == "2") {
                // SELECTIONCYCLING 2: where objects overlap, a list to pick one from.
                auto under = core_.objectsAt(ps);
                if (under.size() > 1) {
                    cycleList_ = std::move(under);
                    cycleListAt_ = ps;
                    openCycleList_ = true;
                } else {
                    core_.click(ps);
                }
            } else if (io.KeyAlt || (cyclingOn && !doubleClick_)) {
                core_.cycleClick(ps);
            } else if (doubleClick_)
                core_.doubleClick(ps);
            else
                core_.click(ps);
            core_.setSelectionDrag(std::nullopt);
            core_.setLassoPath({});
            lassoPath_.clear();
            leftDown_ = dragging_ = false;
        }
    }

    // Right button: what a short click does depends on the mode (Options > User Preferences,
    // SHORTCUTMENU): the context menu, Enter, or repeating the last command.
    // With the wheel on, holding the button past ui.wheel.delayMs shows the tool wheel and
    // releasing over a slice runs it; a shorter press does the click's action on release. The
    // wheel is for starting commands, so it stays closed while one is running.
    const bool wheelOn = flag("ui.wheel.enabled", true);
    auto doRightClick = [&](ui::RightClick a) {
        if (a == ui::RightClick::Menu)
            ImGui::OpenPopup("##viewportMenu");
        else
            core_.submitText("");  // Enter; with no command running it repeats the last one
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const auto* pr = core_.runner().prompt();
        const auto& st = core_.settings();
        ui::RightClickModes modes;
        modes.legacy = st.getString("ui.rightClick", "menu");
        modes.idle = st.getString("ui.rightClick.default", "menu");
        modes.edit = st.getString("ui.rightClick.edit", "menu");
        modes.command = st.getString("ui.rightClick.command", "menu");
        rightAction_ =
            ui::rightClickAction(modes, core_.runner().active(), pr && !pr->keywords.empty(),
                                 !core_.selection().empty());
        if (io.KeyShift && core_.runner().active() && pr &&
            pr->kind == cmd::PromptInfo::Kind::Point)
            ImGui::OpenPopup("##snapMenu");  // Shift + right click: snap overrides
        else if (rightAction_ != ui::RightClick::Enter && wheelOn)
            rightDown_ = true;
        else
            doRightClick(rightAction_);
    }
    if (rightDown_) {
        anyActivity_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            wheel_.close();
            rightDown_ = false;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            const double heldMs = io.MouseDownDuration[ImGuiMouseButton_Right] * 1000.0;
            if (!wheel_.isOpen() && !core_.runner().active() &&
                ui::wheelShouldOpen(heldMs, core_.settings().getNumber("ui.wheel.delayMs",
                                                                       ui::kWheelDefaultDelayMs)))
                wheel_.open(io.MouseClickedPos[ImGuiMouseButton_Right],
                            ui::parseWheelTools(core_.settings().getString("ui.wheel.tools", "")));
        } else {
            const bool wasOpen = wheel_.isOpen();
            const std::string chosen = wheel_.hovered();
            wheel_.close();
            rightDown_ = false;
            if (!wasOpen)
                doRightClick(rightAction_);
            else if (!chosen.empty())
                // The Undo slice undoes one step (U); UNDO itself asks how many.
                core_.submitText(chosen == "UNDO" ? std::string("U") : chosen);
        }
    }
    if (wheel_.isOpen())
        wheel_.draw(io.MousePos,
                    dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0)),
                    theme_);
    viewportMenu();
    cycleListPopup();

    if (hovered || leftDown_ || panning_)
        core_.pointerMoved(ps);
    else
        core_.pointerLeft();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

// Content browser items, Blocks palette tiles and palette tools dragged onto the drawing run as if
// typed.
void Gui::dropIntoViewport(const ViewportRect& rect) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (!pl || !pl->Data || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x < rect.x || m.y < rect.y || m.x >= rect.x + rect.w || m.y >= rect.y + rect.h ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        return;
    const std::string data(static_cast<const char*>(pl->Data));
    std::vector<std::string> lines;
    if (pl->IsDataType(ui::kCommandsPayload))
        lines = ui::toolDropLines(data);
    else if (pl->IsDataType(ui::kContentPayload))
        lines = ui::contentDropLines(ui::decodeContent(data));
    for (const std::string& line : lines)
        core_.submitText(line);
    // A dropped block lands where it was dropped (the insertion point is the drop point); a
    // palette that asks for more (Multiple, on-screen scale) goes on from there.
    const auto* prompt = core_.runner().prompt();
    if (prompt && prompt->kind == cmd::PromptInfo::Kind::Point && prompt->command == "INSERT") {
        const ImGuiIO& io = ImGui::GetIO();
        const double s = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0;
        const ui::PointerState ps{{(m.x - rect.x) * s, (m.y - rect.y) * s}, false, false};
        core_.pointerMoved(ps);
        core_.click(ps);
    }
}

bool Gui::flag(std::string_view key, bool fallback) const {
    const auto v = core_.setting(key);
    return v ? drafting::parseBoolSetting(*v, fallback) : fallback;
}

// Setting hover.tooltip (ROLLOVERTIPS, on unless turned off): after the pointer rests 600 ms on an
// object, a tooltip names its type, colour, layer, linetype and length or radius.
void Gui::hoverTooltip() {
    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    if (std::abs(io.MousePos.x - hoverTipPos_.x) > 2 ||
        std::abs(io.MousePos.y - hoverTipPos_.y) > 2) {
        hoverTipPos_ = io.MousePos;
        hoverTipSince_ = now;
    }
    if (!flag("hover.tooltip", true) || now - hoverTipSince_ < 0.6 || core_.runner().active() ||
        io.WantCaptureMouse)
        return;
    const EntityId id = core_.hoveredEntity();
    if (!id)
        return;
    if (const std::string text = core_.describeEntity(id); !text.empty())
        ImGui::SetTooltip("%s", text.c_str());
}

// What the right-click menu offers depends on the state: a running command (Enter, Cancel, its
// options), a selection (the common edits), or nothing (repeat, select all, undo, zoom).
// The one-point object snap overrides (AutoCAD's Snap Overrides): the next point uses only the
// chosen snap, whatever the running modes are. "None" turns snapping off for that point.
void Gui::snapOverrideItems() {
    const std::string current = core_.setting(drafting::keys::osnapOnce).value_or("");
    for (int i = 0; i < drafting::kSnapModeCount; ++i) {
        const auto mode = static_cast<drafting::SnapMode>(i);
        const std::string name(drafting::snapModeName(mode));
        const std::string label(drafting::snapModeLabel(mode));
        if (ImGui::MenuItem(label.c_str(), nullptr, current == name))
            core_.setSetting(drafting::keys::osnapOnce, name);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("None", nullptr, current == "none"))
        core_.setSetting(drafting::keys::osnapOnce, "none");
    if (!current.empty() && ImGui::MenuItem("Cancel override"))
        core_.setSetting(drafting::keys::osnapOnce, "");
}

void Gui::cycleListPopup() {
    if (openCycleList_) {
        ImGui::OpenPopup("Selection##cycle");
        openCycleList_ = false;
    }
    if (!ImGui::BeginPopup("Selection##cycle")) {
        if (!cycleList_.empty()) {
            cycleList_.clear();
            core_.highlightCandidate({});
        }
        return;
    }
    anyActivity_ = true;
    ImGui::TextDisabled("Selection");
    ImGui::Separator();
    EntityId hovered{};
    for (EntityId id : cycleList_) {
        // "Line, Walls": the type and the layer from the rollover description.
        const std::string d = core_.describeEntity(id);
        std::string label = d.substr(0, d.find('\n'));
        if (const auto at = d.find("\nLayer: "); at != std::string::npos)
            label += ", " + d.substr(at + 8, d.find('\n', at + 8) - (at + 8));
        label += "##" + std::to_string(id.value);
        if (ImGui::Selectable(label.c_str())) {
            core_.pickFromList(id, cycleListAt_);
            cycleList_.clear();
            core_.highlightCandidate({});
            ImGui::CloseCurrentPopup();
            break;
        }
        if (ImGui::IsItemHovered())
            hovered = id;
    }
    if (!cycleList_.empty())
        core_.highlightCandidate(hovered);
    ImGui::EndPopup();
}

void Gui::viewportMenu() {
    if (ImGui::BeginPopup("##snapMenu")) {  // Shift + right click
        anyActivity_ = true;
        ImGui::TextDisabled("Snap overrides (next point)");
        snapOverrideItems();
        ImGui::EndPopup();
    }
    if (!ImGui::BeginPopup("##viewportMenu"))
        return;
    anyActivity_ = true;
    auto run = [&](const std::string& line) { core_.submitText(line); };
    if (core_.runner().active()) {
        if (ImGui::MenuItem("Enter"))
            run("");
        if (ImGui::MenuItem("Cancel"))
            core_.pressEscape();
        if (const auto* prompt = core_.runner().prompt(); prompt && !prompt->keywords.empty()) {
            ImGui::Separator();
            for (const std::string& k : prompt->keywords)
                if (ImGui::MenuItem(k.c_str()))
                    run(k);
        }
        if (const auto* pr = core_.runner().prompt();
            pr && pr->kind == cmd::PromptInfo::Kind::Point) {
            ImGui::Separator();
            if (ImGui::BeginMenu("Snap Overrides")) {
                snapOverrideItems();
                ImGui::EndMenu();
            }
        }
        // The values typed lately (latest first), each once, to answer the prompt again.
        std::vector<std::string> values;
        for (auto it = inputHistory_.lines().rbegin();
             it != inputHistory_.lines().rend() && values.size() < 8; ++it) {
            if (!it->empty() && it->front() != '\'' && !core_.registry().find(*it) &&
                std::find(values.begin(), values.end(), *it) == values.end())
                values.push_back(*it);
        }
        if (!values.empty() && ImGui::BeginMenu("Recent Input")) {
            for (const std::string& v : values)
                if (ImGui::MenuItem(v.c_str()))
                    run(v);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (core_.registry().find("PAN") && ImGui::MenuItem("Pan"))
            run("'PAN");
        if (ImGui::MenuItem("Zoom Extents"))
            core_.zoomExtents();
    } else {
        const std::string last(core_.runner().lastCommand());
        if (!last.empty() && ImGui::MenuItem(("Repeat " + last).c_str()))
            run("");
        // The commands typed lately (latest first), each once; a click runs it.
        std::vector<std::string> recent;
        for (auto it = inputHistory_.lines().rbegin();
             it != inputHistory_.lines().rend() && recent.size() < 8; ++it) {
            std::string word = it->substr(0, it->find(' '));
            std::transform(word.begin(), word.end(), word.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (!word.empty() && word != last && core_.registry().find(word) &&
                std::find(recent.begin(), recent.end(), word) == recent.end())
                recent.push_back(word);
        }
        if (!recent.empty() && ImGui::BeginMenu("Recent Commands")) {
            for (const std::string& r : recent)
                if (ImGui::MenuItem(r.c_str()))
                    run(r);
            ImGui::EndMenu();
        }
        // The user's own commands for this menu (Settings > Customize).
        const std::vector<std::string> mine = core_.settings().getStrings(
            core_.selection().empty() ? ui::kContextMenuKey : ui::kContextMenuEditKey);
        if (!mine.empty()) {
            ImGui::Separator();
            for (std::size_t i = 0; i < mine.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(mine[i].c_str()))
                    run(mine[i]);
                ImGui::PopID();
            }
        }
        if (!core_.selection().empty()) {
            // Edits that suit the kinds of object picked come first.
            bool onlyOne = true;
            bool curves = false;
            std::optional<EntityKind> first;
            for (EntityId id : core_.selection()) {
                const Entity* e = core_.document().find(id);
                if (!e)
                    continue;
                const EntityKind k = kind(*e);
                if (first && *first != k)
                    onlyOne = false;
                first = k;
                curves = curves || k == EntityKind::Line || k == EntityKind::Arc ||
                         k == EntityKind::Polyline || k == EntityKind::Circle ||
                         k == EntityKind::Ellipse || k == EntityKind::Spline;
            }
            auto item = [&](const char* label, const char* command) {
                if (core_.registry().find(command) && ImGui::MenuItem(label))
                    run(command);
            };
            ImGui::Separator();
            if (onlyOne && first) {
                switch (*first) {
                    case EntityKind::Text:
                    case EntityKind::MText:
                        item("Edit Text", "TEXTEDIT");
                        break;
                    case EntityKind::Hatch:
                        item("Edit Hatch", "HATCHEDIT");
                        break;
                    case EntityKind::Polyline:
                        item("Edit Polyline", "PEDIT");
                        item("Explode", "EXPLODE");
                        break;
                    case EntityKind::Insert:
                        item("Edit Block", "BEDIT");
                        item("Explode", "EXPLODE");
                        break;
                    case EntityKind::Dimension:
                        item("Edit Dimension", "DIMEDIT");
                        item("Explode", "EXPLODE");
                        break;
                    default:
                        break;
                }
            }
            item("Cut", "CUTCLIP");
            item("Copy", "COPYCLIP");
            item("Copy with Base Point", "COPYBASE");
            item("Paste", "PASTECLIP");
            item("Paste as Block", "PASTEBLOCK");
            item("Paste to Original Coordinates", "PASTEORIG");
            ImGui::Separator();
            for (const char* c : {"MOVE", "COPY", "ROTATE", "SCALE", "MIRROR", "ERASE"})
                if (core_.registry().find(c) && ImGui::MenuItem(c))
                    run(c);
            if (curves) {
                item("Offset", "OFFSET");
                item("Trim", "TRIM");
                item("Extend", "EXTEND");
            }
            if (core_.registry().find("DRAWORDER") && ImGui::BeginMenu("Draw Order")) {
                // The selection is taken as picked first; the keyword answers the ordering prompt.
                if (ImGui::MenuItem("Bring to Front")) {
                    run("DRAWORDER");
                    run("Front");
                }
                if (ImGui::MenuItem("Send to Back")) {
                    run("DRAWORDER");
                    run("Back");
                }
                if (ImGui::MenuItem("Bring Above Objects")) {
                    run("DRAWORDER");
                    run("Above");
                }
                if (ImGui::MenuItem("Send Under Objects")) {
                    run("DRAWORDER");
                    run("Below");
                }
                ImGui::EndMenu();
            }
            if (core_.selection().size() > 1)
                item("Group", "GROUP");
            item("Ungroup", "UNGROUP");
            ImGui::Separator();
            if (ImGui::MenuItem("Select All"))
                core_.selectAll();
            item("Select Similar", "SELECTSIMILAR");
            item("Quick Select...", "QSELECT");
            item("Match Properties", "MATCHPROP");
            item("Properties", "PROPERTIES");
            if (core_.registry().find("LAYISO") && ImGui::MenuItem("Isolate Layer"))
                run("LAYISO");
            if (ImGui::MenuItem("Deselect All"))
                core_.clearSelection();
        } else {
            if (core_.registry().find("PASTECLIP") && ImGui::MenuItem("Paste"))
                run("PASTECLIP");
            if (ImGui::MenuItem("Select All"))
                core_.selectAll();
            if (core_.registry().find("QSELECT") && ImGui::MenuItem("Quick Select..."))
                run("QSELECT");
            if (core_.registry().find("PROPERTIES") && ImGui::MenuItem("Properties"))
                run("PROPERTIES");
        }
        ImGui::Separator();
        if (core_.registry().find("PAN") && ImGui::MenuItem("Pan"))
            run("PAN");
        const std::string undo = core_.undoLabel();
        const std::string redo = core_.redoLabel();
        if (ImGui::MenuItem(undo.empty() ? "Undo" : undo.c_str(), nullptr, false, !undo.empty()))
            history(false);
        if (ImGui::MenuItem(redo.empty() ? "Redo" : redo.c_str(), nullptr, false, !redo.empty()))
            history(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom Extents"))
            core_.zoomExtents();
    }
    ImGui::EndPopup();
}

// PASTESPEC Image: saves the picture on the system clipboard as a PNG in the settings folder and
// starts IMAGEATTACH on it.
void Gui::pasteClipboardPicture() {
    auto pic = platform::readClipboardPicture();
    if (!pic) {
        core_.print("The clipboard holds no picture.");
        return;
    }
    const std::vector<std::uint8_t> png = mcp::encodePng(pic->rgba, pic->width, pic->height);
    if (png.empty()) {
        core_.print("The clipboard picture could not be converted.");
        return;
    }
    std::error_code ec;
    const std::filesystem::path dir = core_.settingsFolder() / "pasted";
    std::filesystem::create_directories(dir, ec);
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const std::filesystem::path file = dir / ("pasted-" + std::to_string(stamp) + ".png");
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        if (!out) {
            core_.print("Cannot save the clipboard picture to " + platform::pathToUtf8(file));
            return;
        }
    }
    core_.submitText("IMAGEATTACH");
    core_.submitText("\"" + platform::pathToUtf8(file) + "\"");
}

void Gui::queueDroppedFile(std::string path) {
    droppedFiles_.push_back(std::move(path));
}

ViewportRect Gui::frame() {
    for (const std::string& path : droppedFiles_)
        doOpenPath(path);
    droppedFiles_.clear();
    if (auto text = core_.takeClipboardText())
        glfwSetClipboardString(window_, text->c_str());
    shortcuts();
    for (const std::string& what : core_.takeUiRequests()) {
        if (what == "settings")
            settings_.open();
        else if (what.rfind("settings:", 0) == 0)
            settings_.open(what.substr(9));
        else if (what.rfind("action:", 0) == 0)
            runAction(what.substr(7));
        else if (what == "clipboardsnapshot") {
            // PASTESPEC Text/Table waits for an answer: hand it the system clipboard text.
            const char* text = glfwGetClipboardString(window_);
            if (text && *text && std::string_view(text).rfind("[TBCAD objects", 0) != 0) {
                core_.setSetting("tmp.clipboard.system", text);
                core_.submitText("\x01");
            }
        } else if (what == "pasteimage")
            pasteClipboardPicture();
        else if (what.rfind("pickfile:", 0) == 0) {
            const std::string kind = what.substr(9);
            static const std::vector<platform::FileFilter> drawing = {
                {"Drawings and PDF", "*.tbcad;*.dxf;*.dwg;*.pdf"}};
            static const std::vector<platform::FileFilter> pdf = {{"PDF files", "*.pdf"}};
            static const std::vector<platform::FileFilter> image = {
                {"Pictures", "*.png;*.jpg;*.jpeg;*.bmp"}};
            if (platform::nativeDialogsAvailable()) {
                if (auto p = platform::showOpenDialog(nativeHandle(),
                                                      kind == "pdf"     ? pdf
                                                      : kind == "image" ? image
                                                                        : drawing,
                                                      "Choose the file to attach", dialogFolder()))
                    core_.submitText("\"" + platform::pathToUtf8(*p) + "\"");
                // A cancelled dialog leaves the command waiting for a typed path.
            } else {
                core_.print("No file dialog here: type the path.");
            }
        } else if (what.rfind("open:", 0) == 0) {
            // A hyperlink: only after the drafter agrees, and never a program or a script.
            platform::LinkCheck link = platform::checkLink(what.substr(5));
            if (link.kind == platform::LinkKind::Refused) {
                core_.print("Not opened: " + link.reason);
            } else {
                pendingLink_ = std::move(link);
                openLinkPrompt_ = true;
            }
        } else if (what.rfind("openpath:", 0) == 0)
            doOpenPath(what.substr(9));
        else if (what.rfind("recoverpath:", 0) == 0) {
            recoverAfterOpen_ = true;
            doOpenPath(what.substr(12));
        } else if (what.rfind("saveaspath:", 0) == 0) {
            auto r = core_.saveAs(platform::pathFromUtf8(what.substr(11)));
            if (!r)
                lastError_ = r.error().message;
            else
                toast("Saved " + platform::pathToUtf8(core_.file()));
        } else if (what.rfind("workspace:load:", 0) == 0)
            switchWorkspace(what.substr(15));
        else if (what.rfind("workspace:save:", 0) == 0)
            saveWorkspace(what.substr(15));
        else if (what == "realtime:zoom" || what == "realtime:pan")
            realtime_ = what == "realtime:pan" ? Realtime::Pan : Realtime::Zoom;
        else if (what == "cleanscreen:on" || what == "cleanscreen:off")
            cleanScreen_ = what == "cleanscreen:on";
        else if (what == "drawingrecovery")
            recoveryManagerOpen_ = true;
        else if (what.rfind("keys:", 0) == 0) {
            const bool import = what.rfind("keys:import:", 0) == 0;
            const std::string path = what.substr(12);
            const std::string err = import ? core_.importKeybindings(platform::pathFromUtf8(path))
                                           : core_.exportKeybindings(platform::pathFromUtf8(path));
            if (!err.empty())
                core_.print(err);
            else if (!import)
                core_.print("Key bindings written to " + path);
        } else if (what.rfind("newfrom:", 0) == 0) {
            const std::string err = core_.newFromTemplate(what.substr(8));
            if (err.empty())
                startPage_ = false;
            else
                core_.print(err);
        }
    }
    if (!cleanScreen_) {  // F11 hides everything but the viewport and the command line
        menuBar();
        toolStrip();
        docTabs();
        readOnlyBanner();
        blockEditBar();
        contextualBar();
        xrefNotice();  // the list window the bar opens
        statusBar();
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(kDockId, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    dockLayout(kDockId);
    commandWindow();
    if (!cleanScreen_) {
        panels();
    }
    // Focusing a window closes an open popup, so a dialog (the recovery prompt at start-up) wins.
    if (frontLayersIn_ > 0 &&
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        frontLayersIn_ = 0;
    if (frontLayersIn_ > 0 && --frontLayersIn_ == 0)
        ImGui::SetWindowFocus("Layers");
    if (!focusPanel_.empty()) {
        ImGui::SetWindowFocus(focusPanel_.c_str());
        focusPanel_.clear();
    }
    anyActivity_ = anyActivity_ || frontLayersIn_ > 0 || core_.flashing();
    logWindow();

    ViewportRect rect{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
    if (ImGuiDockNode* center = ImGui::DockBuilderGetCentralNode(kDockId)) {
        rect = {center->Pos.x, center->Pos.y, center->Size.x, center->Size.y};
    }
    const float s = ImGui::GetIO().DisplayFramebufferScale.x;
    if (!cleanScreen_ && rect.h > 3 * layoutTabsHeight()) {
        rect.h -= layoutTabsHeight();
        layoutTabs(rect);
    }
    rect = tiledViewports(rect);
    core_.setViewportOrigin(std::lround(rect.x * s), std::lround(rect.y * s));
    core_.setViewportSize(static_cast<int>(std::lround(rect.w * s)),
                          static_cast<int>(std::lround(rect.h * s)));
    viewportInput(rect);
    navigationBar(rect);
    dropIntoViewport(rect);
    lastRect_ = rect;
    startPage(rect);
    cursorHud(rect);
    gripMenu(rect);
    // The drawing origin: a small X (red) / Y (green) axis pair, so a new drawing has a landmark.
    if (!core_.inPaperSpace() && flag("display.originIcon", true) && !ucsIconShown()) {
        const double sc = ImGui::GetIO().DisplayFramebufferScale.x;
        const Vec2 o = core_.camera().worldToScreen({0, 0});
        const ImVec2 at(rect.x + static_cast<float>(o.x / sc),
                        rect.y + static_cast<float>(o.y / sc));
        if (at.x >= rect.x && at.y >= rect.y && at.x < rect.x + rect.w && at.y < rect.y + rect.h) {
            const float len = 40.0f * dpiScale_;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            dl->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.w, rect.y + rect.h));
            dl->AddLine(at, ImVec2(at.x + len, at.y), IM_COL32(230, 80, 80, 220), 1.5f);
            dl->AddLine(at, ImVec2(at.x, at.y - len), IM_COL32(80, 200, 100, 220), 1.5f);
            dl->AddText(ImVec2(at.x + len + 3, at.y - 8), IM_COL32(230, 80, 80, 220), "X");
            dl->AddText(ImVec2(at.x + 4, at.y - len - 16), IM_COL32(80, 200, 100, 220), "Y");
            dl->PopClipRect();
        }
    }
    if (const auto n = core_.dragSelectCount(); n && (*n > 0 || core_.inPaperSpace())) {
        const ImVec2 at(ImGui::GetIO().MousePos.x + 16 * dpiScale_,
                        ImGui::GetIO().MousePos.y + 20 * dpiScale_);
        const std::string label = std::to_string(*n) + (*n == 1 ? " object" : " objects");
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 sz = ImGui::CalcTextSize(label.c_str());
        dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + sz.x + 4, at.y + sz.y + 2),
                          IM_COL32(20, 22, 26, 200), 4.0f);
        dl->AddText(at, IM_COL32(230, 232, 235, 255), label.c_str());
    }
    hoverTooltip();
    quickProperties(rect);
    textEditor(rect);
    drawToasts(rect);
    drawXrefBalloon(rect);

    // Non-blocking notice when a command put geometry outside the working area.
    if (core_.outOfAreaNotice()) {
        ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w * 0.5f, rect.y + 12), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        const ImGuiWindowFlags nf = ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##outofarea", nullptr, nf)) {
            ImGui::Text("%zu object(s) lie outside the working area.",
                        core_.outOfAreaNotice()->ids.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Zoom to"))
                core_.outOfAreaZoomTo();
            ImGui::SameLine();
            if (ImGui::SmallButton("Undo"))
                core_.outOfAreaUndo();
            ImGui::SameLine();
            if (ImGui::SmallButton("Dismiss"))
                core_.dismissOutOfAreaNotice();
        }
        ImGui::End();
    }
    // Label of the active snap point or tracking ray, next to the cursor.
    const ImGuiIO& tipIo = ImGui::GetIO();
    const bool tipInside = tipIo.MousePos.x >= rect.x && tipIo.MousePos.y >= rect.y &&
                           tipIo.MousePos.x < rect.x + rect.w && tipIo.MousePos.y < rect.y + rect.h;
    if (tipInside && core_.feedback().active && !core_.feedback().tooltip.empty()) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 at(tipIo.MousePos.x + 16, tipIo.MousePos.y + 20);
        const ImVec2 size = ImGui::CalcTextSize(core_.feedback().tooltip.c_str());
        fg->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + size.x + 4, at.y + size.y + 2),
                          IM_COL32(24, 26, 30, 220), 3.0f);
        fg->AddText(at, IM_COL32(242, 169, 59, 255), core_.feedback().tooltip.c_str());
    }
    dialogs();
    closeTopModalOnEscape();
    return rect;
}

// Tiled model viewports (VPORTS): splits the drawing area, makes a clicked tile the active one and
// returns the active tile's rectangle (the one the pointer and the commands work in). The other
// tiles are listed in inactiveTiles_ for the window to draw.
ViewportRect Gui::tiledViewports(const ViewportRect& area) {
    inactiveTiles_.clear();
    if (!core_.tiled())
        return area;
    const auto& tiles = core_.tiles();
    auto rectOf = [&](const ui::AppCore::ViewTile& t) {
        const float gap = 1.0f;
        return ViewportRect{area.x + static_cast<float>(t.x0) * area.w + (t.x0 > 0 ? gap : 0),
                            area.y + static_cast<float>(t.y0) * area.h + (t.y0 > 0 ? gap : 0),
                            static_cast<float>(t.x1 - t.x0) * area.w - (t.x0 > 0 ? gap : 0),
                            static_cast<float>(t.y1 - t.y0) * area.h - (t.y0 > 0 ? gap : 0)};
    };
    // A click (any button) or the wheel over another tile makes it active first.
    const ImGuiIO& io = ImGui::GetIO();
    const bool pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || io.MouseWheel != 0;
    if (pressed && !io.WantCaptureMouse && !core_.runner().active())
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const ViewportRect r = rectOf(tiles[i]);
            if (i != core_.activeTile() && io.MousePos.x >= r.x && io.MousePos.y >= r.y &&
                io.MousePos.x < r.x + r.w && io.MousePos.y < r.y + r.h) {
                const float sc = io.DisplayFramebufferScale.x;
                core_.activateTile(i, std::lround(r.x * sc), std::lround(r.y * sc),
                                   static_cast<int>(std::lround(r.w * sc)),
                                   static_cast<int>(std::lround(r.h * sc)));
                tileJustActivated_ = true;  // that click only activates
                break;
            }
        }
    ViewportRect active = area;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const ViewportRect r = rectOf(tiles[i]);
        if (i == core_.activeTile())
            active = r;
        else
            inactiveTiles_.push_back({i, r});
    }
    return active;
}

void Gui::showTile(unsigned texture, const ViewportRect& rect) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    bg->AddImage(static_cast<ImTextureID>(texture), ImVec2(rect.x, rect.y),
                 ImVec2(rect.x + rect.w, rect.y + rect.h), ImVec2(0, 1), ImVec2(1, 0));
    bg->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.w, rect.y + rect.h),
                IM_COL32(90, 95, 105, 255));
}

// The navigation bar (NAVBAR): pan, zoom extents / window / previous, zoom in and out, at the
// right edge of the drawing.
void Gui::navigationBar(const ViewportRect& rect) {
    if (cleanScreen_ || core_.setting("ui.navbar").value_or("1") == "0" || rect.w < 200 ||
        rect.h < 260)
        return;
    const float pad = 8.0f * dpiScale_;
    ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w - pad, rect.y + rect.h * 0.5f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                               ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##navbar", nullptr, f)) {
        const ImVec2 size(64.0f * dpiScale_, 0);
        auto button = [&](const char* label, const char* tip) {
            const bool hit = ImGui::Button(label, size);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tip);
            return hit;
        };
        const bool panOn = realtime_ == Realtime::Pan;
        if (panOn)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (button("Pan", "Real-time pan: drag the drawing (PAN). Esc ends it."))
            realtime_ = panOn ? Realtime::Off : Realtime::Pan;
        if (panOn)
            ImGui::PopStyleColor();
        if (button("Extents", "Zoom to the whole drawing (ZOOM Extents)"))
            core_.submitText("'ZOOM E");
        if (button("Window", "Zoom to a window you drag (ZOOM Window)"))
            core_.submitText("'ZOOM W");
        if (button("Previous", "Back to the view before (ZOOM Previous)"))
            core_.submitText("'ZOOM P");
        if (button("In", "Zoom in, twice as big (ZOOM 2X)"))
            core_.submitText("'ZOOM 2X");
        if (button("Out", "Zoom out, half as big (ZOOM 0.5X)"))
            core_.submitText("'ZOOM 0.5X");
        if (button("Real time", "Real-time zoom: drag up or down (ZOOM). Esc ends it."))
            realtime_ = Realtime::Zoom;
    }
    ImGui::End();
}

void Gui::showViewport(unsigned texture, const ViewportRect& rect) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    bg->AddImage(static_cast<ImTextureID>(texture), ImVec2(rect.x, rect.y),
                 ImVec2(rect.x + rect.w, rect.y + rect.h), ImVec2(0, 1), ImVec2(1, 0));
    ucsIcon(rect);  // after the picture, so it lies on top of the drawing
}

bool Gui::ucsIconShown() const {
    // The start page covers an untouched drawing; the icon waits for the first change.
    const bool startPageShown =
        startPage_ && core_.isPristine() && core_.documentCount() == 1 && !core_.runner().active();
    return !startPageShown && !core_.inPaperSpace() &&
           drafting::parseBoolSetting(core_.setting(drafting::keys::ucsIcon).value_or("1"), true);
}

// The coordinate-system icon (UCSICON): an X and a Y arrow along the axes of the current UCS, at
// the UCS origin or, with "Noorigin" or when the origin is off screen, in the lower-left corner.
// A "W" next to it says the system is the world system; a small square marks the real origin.
void Gui::ucsIcon(const ViewportRect& rect) {
    if (!ucsIconShown())
        return;
    const drafting::Ucs ucs = drafting::currentUcs(&core_.document(), &core_);
    const double sc = ImGui::GetIO().DisplayFramebufferScale.x;
    // UCSICON Properties: size 5-95 (50 = the normal 36 px) and one colour or the default pair.
    int sizeSetting = std::atoi(core_.setting(drafting::keys::ucsIconSize).value_or("50").c_str());
    if (sizeSetting < 5 || sizeSetting > 95)
        sizeSetting = 50;
    const float len = 36.0f * dpiScale_ * static_cast<float>(sizeSetting) / 50.0f;
    const float inset = std::max(44.0f * dpiScale_, len + 8.0f * dpiScale_);
    ImVec2 at(rect.x + inset, rect.y + rect.h - inset);
    bool atOrigin = false;
    if (drafting::parseBoolSetting(core_.setting(drafting::keys::ucsIconOrigin).value_or("1"),
                                   true)) {
        const Vec2 o = core_.camera().worldToScreen(ucs.origin);
        const ImVec2 p(rect.x + static_cast<float>(o.x / sc),
                       rect.y + static_cast<float>(o.y / sc));
        const float margin = len + 20.0f * dpiScale_;  // the whole icon stays inside the viewport
        if (p.x >= rect.x + margin * 0.3f && p.y >= rect.y + margin &&
            p.x < rect.x + rect.w - margin && p.y < rect.y + rect.h - margin * 0.3f) {
            at = p;
            atOrigin = true;
        }
    }
    const float c = static_cast<float>(std::cos(ucs.angle));
    const float sn = static_cast<float>(std::sin(ucs.angle));
    const ImVec2 dx(c, -sn), dy(-sn, -c);  // screen directions of the UCS axes (y grows downwards)
    ImU32 xCol = IM_COL32(230, 80, 80, 235), yCol = IM_COL32(80, 200, 100, 235);
    const int colourIndex =
        std::atoi(core_.setting(drafting::keys::ucsIconColor).value_or("Default").c_str());
    if (colourIndex >= 1 && colourIndex <= 255) {
        const Rgba rgb = paletteColor(static_cast<std::uint8_t>(colourIndex));
        xCol = yCol = IM_COL32(rgb.r, rgb.g, rgb.b, 235);
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    dl->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.w, rect.y + rect.h));
    auto arrow = [&](ImVec2 d, ImU32 col, const char* name) {
        const ImVec2 tip(at.x + d.x * len, at.y + d.y * len);
        const ImVec2 n(-d.y, d.x);
        const float h = 7.0f * dpiScale_;
        dl->AddLine(at, tip, col, 1.6f);
        dl->AddTriangleFilled(tip,
                              ImVec2(tip.x - d.x * h * 1.6f + n.x * h * 0.5f,
                                     tip.y - d.y * h * 1.6f + n.y * h * 0.5f),
                              ImVec2(tip.x - d.x * h * 1.6f - n.x * h * 0.5f,
                                     tip.y - d.y * h * 1.6f - n.y * h * 0.5f),
                              col);
        const ImVec2 ts = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(tip.x + d.x * 8.0f * dpiScale_ - ts.x * 0.5f,
                           tip.y + d.y * 8.0f * dpiScale_ - ts.y * 0.5f),
                    col, name);
    };
    arrow(dx, xCol, "X");
    arrow(dy, yCol, "Y");
    if (atOrigin) {
        const float q = 3.5f * dpiScale_;
        dl->AddRect(ImVec2(at.x - q, at.y - q), ImVec2(at.x + q, at.y + q),
                    IM_COL32(140, 145, 155, 240));
    }
    if (ucs.isWorld())
        dl->AddText(ImVec2(at.x + 6.0f * dpiScale_, at.y + 3.0f * dpiScale_),
                    IM_COL32(140, 145, 155, 240), "W");
    dl->PopClipRect();
}

// ---- dialogs -----------------------------------------------------------------------------------

// ImGui leaves modal dialogs open on Escape. Dialogs that need a decision handle the key
// themselves; every other modal closes here, unless a text box is being edited (Esc then
// belongs to the box).
void Gui::closeTopModalOnEscape() {
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsAnyItemActive())
        return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = g.OpenPopupStack.Size - 1; i >= 0; --i) {
        const ImGuiWindow* w = g.OpenPopupStack[i].Window;
        if (w && (w->Flags & ImGuiWindowFlags_Modal)) {
            ImGui::ClosePopupToLevel(i, true);
            return;
        }
        if (w && !(w->Flags & ImGuiWindowFlags_Popup))
            return;
    }
}

void Gui::recoveryManager() {
    if (!recoveryManagerOpen_)
        return;
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Drawing Recovery", &recoveryManagerOpen_)) {
        ImGui::End();
        return;
    }
    // Read from disk at most once a second: the window stays open while the user works.
    static std::vector<ui::Recovery> autosaves, backups;
    static double readAt = -10;
    if (ImGui::GetTime() - readAt > 1.0) {
        autosaves = core_.leftoverRecoveries();
        backups = core_.backupFiles();
        readAt = ImGui::GetTime();
    }
    auto table = [&](const char* id, std::vector<ui::Recovery>& rows, bool autosave) {
        if (rows.empty()) {
            ImGui::TextDisabled(autosave ? "No autosaves are waiting." : "No backup files found.");
            return;
        }
        if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            return;
        ImGui::TableSetupColumn("Drawing");
        ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const ui::Recovery& r = rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string name =
                r.originalPath.empty()
                    ? std::string("Untitled (never saved)")
                    : platform::pathToUtf8(platform::pathFromUtf8(r.originalPath).filename());
            ImGui::TextUnformatted(name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", platform::pathToUtf8(r.autosaveFile).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.savedAt.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Open")) {
                if (auto res = core_.openRecovered(r, autosave); !res)
                    lastError_ = res.error().message;
                readAt = -10;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    };
    ImGui::SeparatorText("Autosaves of drawings that were not closed normally");
    table("##autosaves", autosaves, true);
    ImGui::SeparatorText("Backup files (.bak, the drawing before its last save)");
    table("##backups", backups, false);
    ImGui::Spacing();
    if (!autosaves.empty() && ImGui::Button("Discard all autosaves")) {
        for (const ui::Recovery& r : autosaves) {
            std::error_code ec;
            std::filesystem::path meta = r.autosaveFile;
            meta.replace_extension(".json");
            std::filesystem::remove(r.autosaveFile, ec);
            std::filesystem::remove(meta, ec);
        }
        readAt = -10;
    }
    ImGui::End();
}

void Gui::dialogs() {
    openPending();
    historyPending();
    settings_.draw(core_);
    workspaceDialog();
    recoveryManager();
    if (openRecoveryPrompt_) {
        ImGui::OpenPopup("Recover drawing?");
        openRecoveryPrompt_ = false;
    }
    if (openDxfVersionPrompt_) {
        ImGui::OpenPopup("Save as DXF");
        openDxfVersionPrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Save as DXF", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("DXF version:");
        bool changed = false;
        changed |= ImGui::RadioButton("R12 (AC1009) - oldest, simplest", &dxfVersionChoice_, 0);
        changed |= ImGui::RadioButton("R2000 (AC1015) - read by everything", &dxfVersionChoice_, 1);
        changed |=
            ImGui::RadioButton("R2007 and later (AC1021) - UTF-8 text", &dxfVersionChoice_, 2);
        if (changed)
            dxfVersionWarnings_ = core_.dxfLossWarnings(dxfChoice(dxfVersionChoice_));
        ImGui::Spacing();
        if (dxfVersionWarnings_.empty()) {
            ImGui::TextDisabled("Nothing in this drawing is lost in this version.");
        } else {
            ImGui::Text("Lost or simplified in this version (%zu):", dxfVersionWarnings_.size());
            ImGui::BeginChild("##dxfloss", ImVec2(520, 140), true);
            ImGui::PushTextWrapPos(0);
            for (const std::string& w : dxfVersionWarnings_)
                ImGui::BulletText("%s", w.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            static constexpr const char* kVer[] = {"r12", "r2000", "r2007"};
            core_.settings().setString("files.dxfVersion", kVer[dxfVersionChoice_]);
            auto r = core_.saveAs(pendingDxfPath_);
            if (!r)
                lastError_ = r.error().message;
            else {
                toast("Saved " + platform::pathToUtf8(core_.file()));
                if (saveThenPending_)
                    performPending(false);
            }
            saveThenPending_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            saveThenPending_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openLinkPrompt_) {
        ImGui::OpenPopup("Open link?");
        openLinkPrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Open link?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        using platform::LinkKind;
        ImGui::TextUnformatted(pendingLink_.kind == LinkKind::Web
                                   ? "This drawing links to a web address:"
                               : pendingLink_.kind == LinkKind::Mail
                                   ? "This drawing links to a mail address:"
                                   : "This drawing links to a file on this computer:");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(pendingLink_.target.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("Only open links you trust.");
        const bool enterKey =
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        if (ImGui::Button("Open", ImVec2(110, 0)) || enterKey) {
            if (!platform::openExternal(pendingLink_))
                core_.print("Could not open " + pendingLink_.target);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Recover drawing?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TBCAD did not close normally last time.");
        if (recoveryCount_ > 1)
            ImGui::Text("Automatic backups of %zu unsaved drawings were found.", recoveryCount_);
        else if (recovery_)
            ImGui::TextUnformatted("An automatic backup of an unsaved drawing was found.");
        if (recovery_ && !recovery_->originalPath.empty())
            ImGui::Text("Original file: %s", recovery_->originalPath.c_str());
        if (recovery_ && !recovery_->savedAt.empty() && recoveryCount_ <= 1)
            ImGui::Text("Backup saved: %s", recovery_->savedAt.c_str());
        // The keyboard works too: a modal that only answers to the mouse is stuck when the
        // window has not been clicked yet. Enter recovers (or says OK), Esc decides later.
        const bool enterKey =
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        const bool escKey = ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (!crashReport_.empty()) {
            ImGui::TextWrapped(
                "A crash report was saved to %s. Attach it when you report the problem.",
                crashReport_.c_str());
            if (!recovery_) {
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(110, 0)) || enterKey) {
                    platform::acknowledgeCrashReports(core_.paths().crashDir());
                    crashReport_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::Spacing();
        if (recovery_ && (ImGui::Button("Recover", ImVec2(110, 0)) || enterKey)) {
            platform::acknowledgeCrashReports(core_.paths().crashDir());
            crashReport_.clear();
            if (recovery_) {
                auto r = core_.recoverAll();
                if (!r)
                    lastError_ = r.error().message;
            }
            recovery_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (recovery_ && ImGui::Button("Discard", ImVec2(110, 0))) {
            platform::acknowledgeCrashReports(core_.paths().crashDir());
            crashReport_.clear();
            core_.discardRecovery();
            recovery_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(110, 0)) || escKey) {
            // Nothing is deleted: the backup is offered again next time.
            crashReport_.clear();
            recovery_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openSavePrompt_) {
        ImGui::OpenPopup("Save changes?");
        openSavePrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s?", core_.displayName().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            if (core_.hasFile()) {
                auto r = core_.save();
                if (r)
                    performPending(false);
                else {
                    lastError_ = r.error().message;
                    pending_ = Pending::None;
                }
            } else {
                saveThenPending_ = true;
                doSaveAs();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            performPending(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            pending_ = Pending::None;
            saveThenPending_ = false;
        }
        ImGui::EndPopup();
    }

    if (!lastError_.empty()) {
        ImGui::OpenPopup("Error");
        errorText_ = std::move(lastError_);
        lastError_.clear();
    }
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", errorText_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showAbout_) {
        ImGui::OpenPopup("About TBCAD");
        showAbout_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About TBCAD", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("TBCAD %s", TBCAD_VERSION);
        ImGui::TextDisabled("Build %s", TBCAD_GIT_HASH);
        ImGui::TextUnformatted("A free 2D CAD program.");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Copyright (c) TBCAD contributors. TBCAD is released under the MIT License: you may "
            "use, copy, modify and distribute it, including in programs whose own source code you "
            "do not publish, provided the copyright and license notice stay with it. The software "
            "is provided \"as is\", without warranty of any kind.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Third-party software:");
        ImGui::BulletText("Dear ImGui (MIT)");
        ImGui::BulletText("GLFW (zlib/libpng)");
        ImGui::BulletText("FreeType (FreeType License) and HarfBuzz (MIT)");
        ImGui::BulletText("miniz (MIT), earcut (ISC), glad (CC0)");
        ImGui::BulletText("Lato, Sarabun and Liberation Mono fonts (SIL Open Font License)");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Portions of this software are copyright \xC2\xA9 The FreeType Project "
            "(www.freetype.org). All rights reserved.");
        ImGui::Spacing();
        if (ImGui::Button("Third-Party Notices..."))
            showNotices_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(110, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showReport_) {
        ImGui::OpenPopup("Report a Problem");
        showReport_ = false;
    }
    if (ImGui::BeginPopupModal("Report a Problem", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string details =
            std::string("TBCAD ") + TBCAD_VERSION + " (build " + TBCAD_GIT_HASH + ")";
        const std::string crashDir = platform::pathToUtf8(core_.paths().crashDir());
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(
            "Found something wrong? Tell us what you were doing, and attach the drawing if you "
            "can. If the program crashed, attach the newest file from the crash folder.");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", details.c_str());
        ImGui::TextDisabled("Crash folder: %s", crashDir.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Copy version info", ImVec2(160, 0)))
            ImGui::SetClipboardText(details.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Open crash folder", ImVec2(160, 0))) {
            std::error_code ec;
            std::filesystem::create_directories(core_.paths().crashDir(), ec);
            auto link = platform::checkLink(crashDir + "/");
            if (!platform::openExternal(link))
                core_.print("Could not open " + crashDir);
        }
        if (ImGui::Button("Open problem page", ImVec2(160, 0))) {
            ImGui::SetClipboardText(details.c_str());
            auto link = platform::checkLink("https://github.com/snabs1993/TBCAD/issues/new");
            if (!platform::openExternal(link))
                core_.print("Could not open the problem page");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(160, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showQuickStart_) {
        ImGui::OpenPopup("Quick Start");
        showQuickStart_ = false;
        if (quickStartText_.empty()) {
            std::ifstream in(findBesideExecutable("assets/help/quickstart.md"), std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            quickStartText_ = in ? buf.str() : "The quick start guide was not found (assets/help).";
        }
    }
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Quick Start", nullptr)) {
        ImGui::BeginChild("##quickstart", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        ImGui::PushTextWrapPos(0.0f);
        drawMarkdownLite(quickStartText_);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(110, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!helpCommand_.empty()) {
        ImGui::OpenPopup("Command Help");
        helpShown_ = helpCommand_;
        helpCommand_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Command Help", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const cmd::Command* c = core_.registry().find(helpShown_);
        ImGui::TextUnformatted(helpShown_.c_str());
        if (c) {
            std::string aliases;
            for (const std::string& a : core_.registry().aliasesOf(c->name()))
                aliases += (aliases.empty() ? "" : ", ") + a;
            if (!aliases.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", aliases.c_str());
            }
            ImGui::Separator();
            ImGui::PushTextWrapPos(500.0f);
            ImGui::TextUnformatted(std::string(c->description()).c_str());
            ImGui::PopTextWrapPos();
        }
        if (const auto* prompt = core_.runner().prompt()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Right now");
            ImGui::PushTextWrapPos(500.0f);
            ImGui::TextUnformatted(prompt->text.c_str());
            ImGui::PopTextWrapPos();
            if (!prompt->keywords.empty()) {
                std::string opts;
                for (const std::string& k : prompt->keywords)
                    opts += (opts.empty() ? "" : "  ") + std::string("[") + k + "]";
                ImGui::TextDisabled("Options: %s", opts.c_str());
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("All commands...")) {
            showCommandList_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showCommandList_) {
        ImGui::OpenPopup("Command List");
        showCommandList_ = false;
        commandFilter_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Command List", nullptr)) {
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##cmdfilter", "Type to search commands", &commandFilter_);
        ImGui::SameLine();
        ImGui::TextDisabled("Click a command to run it.");
        ImGui::BeginChild("##cmdlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        if (ImGui::BeginTable("##cmdtable", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Command");
            ImGui::TableSetupColumn("Shortcut");
            ImGui::TableSetupColumn("What it does", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            auto lower = [](std::string t) {
                for (char& c : t)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return t;
            };
            const std::string needle = lower(commandFilter_);
            for (cmd::Command* c : core_.registry().list()) {
                if (c->description().empty())
                    continue;
                std::string aliases;
                for (const std::string& a : core_.registry().aliasesOf(c->name()))
                    aliases += (aliases.empty() ? "" : ", ") + a;
                const std::string name(c->name());
                const std::string desc(c->description());
                if (!needle.empty() &&
                    lower(name + " " + aliases + " " + desc).find(needle) == std::string::npos)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (core_.runner().active())
                        core_.pressEscape();
                    core_.submitText(name);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(aliases.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", desc.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(110, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showNotices_) {
        ImGui::OpenPopup("Third-Party Notices");
        showNotices_ = false;
        if (!noticesLoaded_) {
            noticesLoaded_ = true;
            std::string path = findBesideExecutable("THIRD_PARTY_NOTICES.txt");
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            noticesText_ =
                in ? buf.str() : "THIRD_PARTY_NOTICES.txt was not found next to the program.";
        }
    }
    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Third-Party Notices", nullptr)) {
        ImGui::BeginChild("##notices", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(noticesText_.c_str());
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(110, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (browser_.isOpen()) {
        if (auto path = browser_.draw()) {
            if (browserPurpose_ == BrowserPurpose::Open) {
                doOpenPath(platform::pathToUtf8(*path));
            } else if (browserPurpose_ == BrowserPurpose::ImportDxf) {
                dxfCommandWithPath("IMPORTDXF", *path);
            } else if (browserPurpose_ == BrowserPurpose::LayoutTemplate) {
                core_.submitText("LAYOUT");
                core_.submitText("Template");
                core_.submitText(platform::pathToUtf8(*path));
            } else if (browserPurpose_ == BrowserPurpose::ExportDxf) {
                dxfCommandWithPath("EXPORTDXF", *path);
            } else {
                saveAsPath(*path);
            }
        }
    }
}

}  // namespace tbcad::app
