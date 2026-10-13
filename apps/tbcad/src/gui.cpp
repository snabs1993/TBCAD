#include "gui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "icons.h"
#include "key_map.h"
#include "tbcad/platform/crash.h"
#include "tbcad/platform/file_dialog.h"
#include "tbcad/platform/paths.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/drag_payload.h"
#include "tbcad/ui/selection.h"

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

const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
            return "Line";
        case EntityKind::Circle:
            return "Circle";
        case EntityKind::Arc:
            return "Arc";
        case EntityKind::Polyline:
            return "Polyline";
        case EntityKind::Point:
            return "Point";
        case EntityKind::Ellipse:
            return "Ellipse";
        case EntityKind::Spline:
            return "Spline";
        case EntityKind::Text:
            return "Text";
        case EntityKind::MText:
            return "Multiline text";
        case EntityKind::Hatch:
            return "Hatch";
        case EntityKind::Insert:
            return "Block reference";
        case EntityKind::Dimension:
            return "Dimension";
        case EntityKind::Leader:
            return "Leader";
        case EntityKind::Proxy:
            return "Proxy object";
        case EntityKind::Table:
            return "Table";
        case EntityKind::Ray:
            return "Ray";
        case EntityKind::MLine:
            return "Multiline";
    }
    return "Object";
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
          [this](const std::string& action) { return actionLabel(action); }}),
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
    lineweights_ = core_.settings().getBool("view.lineweights", true);
    cursorHud_ = core_.settings().getBool("hud.enabled", true);
    for (const auto& p : registry_.panels())
        panelOpen_[p.id] = p.visibleByDefault;
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
        {"view.toggleCommandLine", "View > Command Line"},
        {"view.expandLog", "View > Command Log"},
        {"view.cleanScreen", "View > Clean Screen"},
        {"help.quickStart", "Help > Quick Start"},
        {"tools.settings", "Tools > Settings"}};
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
    if (action == "file.new") {
        core_.newDocument();
        startPage_ = false;
    } else if (action == "file.open")
        doOpen();
    else if (action == "file.close")
        requestAction(Pending::Close);
    else if (action == "view.nextDocument")
        core_.activateDocument((core_.activeDocument() + 1) % core_.documentCount());
    else if (action == "file.save")
        doSave();
    else if (action == "file.saveAs")
        doSaveAs();
    else if (action == "edit.undo")
        core_.undo();
    else if (action == "edit.redo")
        core_.redo();
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
    else if (action == "view.cleanScreen")
        cleanScreen_ = !cleanScreen_;
    else if (action == "help.quickStart")
        showQuickStart_ = true;
    else if (action == "tools.settings")
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

// Closing one drawing, or leaving the program: asks about each drawing with unsaved changes in
// turn (it becomes the active tab so the user sees what is being asked about).
void Gui::continuePending() {
    if (pending_ == Pending::Close) {
        if (core_.modified())
            openSavePrompt_ = true;
        else
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

void Gui::doOpen() {
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showOpenDialog(nativeHandle(), openFilters(), "Open drawing"))
            doOpenPath(platform::pathToUtf8(*p));
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
        if (auto p = platform::showOpenDialog(nativeHandle(), filters, "Import DXF"))
            dxfCommandWithPath("IMPORTDXF", *p);
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::ImportDxf;
    browser_.open(FileBrowser::Mode::Open, start, "", ".dxf");
}

void Gui::doExportDxf() {
    static const std::vector<platform::FileFilter> filters = {{"DXF drawings", "*.dxf"}};
    std::filesystem::path stem = core_.hasFile() ? core_.file() : std::filesystem::path{"Drawing1"};
    stem.replace_extension(".dxf");
    std::string suggested = stem.filename().string();
    if (platform::nativeDialogsAvailable()) {
        if (auto p =
                platform::showSaveDialog(nativeHandle(), filters, "Export DXF", "dxf", suggested))
            dxfCommandWithPath("EXPORTDXF", *p);
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::ExportDxf;
    browser_.open(FileBrowser::Mode::Save, start, suggested, ".dxf");
}

void Gui::doOpenPath(const std::string& path) {
    auto r = core_.openFile(platform::pathFromUtf8(path));
    if (!r)
        lastError_ = r.error().message;
}

void Gui::doSave() {
    if (!core_.hasFile() || core_.readOnlyOrigin()) {
        doSaveAs();
        return;
    }
    auto r = core_.save();
    if (!r)
        lastError_ = r.error().message;
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
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showSaveDialog(nativeHandle(), drawingFilters(ext == "dxf"),
                                              "Save drawing as", ext, suggested)) {
            auto r = core_.saveAs(*p);
            if (!r)
                lastError_ = r.error().message;
            else if (saveThenPending_)
                performPending(false);
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
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
         ImGui::IsKeyPressed(ImGuiKey_Space)))
        core_.submitText("");
}

// ---- menus -------------------------------------------------------------------------------------

namespace {

void drawNode(const MenuNode& node, ui::AppCore& core, const platform::Keybindings& keys) {
    for (const MenuNode& child : node.children) {
        if (child.item) {
            std::string shortcut =
                child.item->actionId.empty() ? "" : keys.label(child.item->actionId);
            if (ImGui::MenuItem(child.name.c_str(), shortcut.c_str()))
                core.submitText(child.item->command);
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
        if (ImGui::MenuItem("Exit"))
            requestAction(Pending::Exit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        std::string undoLabel = core_.undoLabel();
        std::string redoLabel = core_.redoLabel();
        if (ImGui::MenuItem(undoLabel.empty() ? "Undo" : undoLabel.c_str(),
                            keys.label("edit.undo").c_str(), false, !undoLabel.empty()))
            core_.undo();
        if (ImGui::MenuItem(redoLabel.empty() ? "Redo" : redoLabel.c_str(),
                            keys.label("edit.redo").c_str(), false, !redoLabel.empty()))
            core_.redo();
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
            ImGui::MenuItem(p.title.c_str(), nullptr, &panelOpen_[p.id]);
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
        if (ImGui::MenuItem("Show Line Weights", nullptr, &lineweights_))
            core_.settings().setBool("view.lineweights", lineweights_);
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
        for (const Group& group : groups) {
            const std::string key = std::string("ui.toolgroup.") + group.name;
            if (!core_.settings().getBool(key, true))
                continue;
            bool any = false;
            for (const Tool& t : group.tools)
                any = any || t.action || core_.registry().find(t.command);
            if (!any)
                continue;
            if (!first) {
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(p.x - ImGui::GetStyle().ItemSpacing.x, p.y + 3),
                    ImVec2(p.x - ImGui::GetStyle().ItemSpacing.x, p.y + iconSize - 3),
                    ImGui::GetColorU32(ImGuiCol_Separator));
            }
            first = false;
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
                    else
                        core_.submitText(t.command);
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                                         ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", t.tip);
            }
        }
        // Right-click the strip to show or hide a group.
        if (ImGui::BeginPopupContextWindow("##toolstripMenu")) {
            for (const Group& group : groups) {
                const std::string key = std::string("ui.toolgroup.") + group.name;
                bool shown = core_.settings().getBool(key, true);
                if (ImGui::MenuItem(group.name, nullptr, &shown))
                    core_.settings().setBool(key, shown);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void Gui::readOnlyBanner() {
    const std::string text = core_.readOnlyBanner();
    if (text.empty())
        return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 0.8f;
    if (ImGui::BeginViewportSideBar("##readonlybanner", vp, ImGuiDir_Up, h, flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Save As..."))
            runAction("file.saveAs");
    }
    ImGui::End();
}

void Gui::statusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 0.8f;
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, h, flags)) {
        const Document& doc = core_.document();
        UnitFormat fmt;
        fmt.unit = doc.header().units;
        Vec2 c = core_.cursorWorld();
        ImGui::Text("X %s   Y %s   %s", formatLength(c.x, 3, fmt).c_str(),
                    formatLength(c.y, 3, fmt).c_str(), std::string(unitSuffix(fmt.unit)).c_str());
        for (const auto& t : registry_.statusToggles()) {
            ImGui::SameLine();
            bool on = t.get();
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::SmallButton(t.label.c_str()))
                t.set(!on);
            if (on)
                ImGui::PopStyleColor();
        }
        const Layer* layer = doc.layer(doc.header().currentLayer);
        ImGui::SameLine();
        ImGui::Text("  Layer: %s", layer ? layer->name.c_str() : "0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        if (ImGui::BeginCombo("##workspace",
                              workspaceName_.empty() ? "Workspace" : workspaceName_.c_str(),
                              ImGuiComboFlags_HeightLarge)) {
            for (const std::string& name : workspaces_.names())
                if (ImGui::Selectable(name.c_str(), name == workspaceName_))
                    switchWorkspace(name);
            ImGui::EndCombo();
        }

        // Level switcher: pick the level to work on (highest first, like a section).
        {
            const Level* cur = doc.level(doc.header().currentLevel);
            ImGui::SameLine();
            ImGui::TextUnformatted("  Level:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("Level 00 (mmm)").x + 40.0f);
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

        std::string right = std::to_string(doc.modelSpaceCount()) + " objects   " +
                            std::to_string(core_.selection().size()) + " selected   " +
                            std::to_string(static_cast<int>(fps_ + 0.5)) + " fps";
        float w = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextUnformatted(right.c_str());
    }
    ImGui::End();
}

// ---- windows -----------------------------------------------------------------------------------

void Gui::dockLayout(ImGuiID ds) {
    ImGuiDockNode* existing = ImGui::DockBuilderGetNode(ds);
    if (existing && (existing->IsSplitNode() || existing->Windows.Size > 0) && !rebuildDock_)
        return;  // restored from imgui.ini
    rebuildDock_ = false;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(ds);
    ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ds, vp->WorkSize);
    ImGuiID center = ds;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Command", bottom);
    for (const auto& p : registry_.panels())
        ImGui::DockBuilderDockWindow(p.title.c_str(), right);
    ImGui::DockBuilderFinish(ds);
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
                suggestSel_ = d->EventKey == ImGuiKey_UpArrow ? std::max(0, suggestSel_ - 1)
                                                              : std::min(n - 1, suggestSel_ + 1);
                return 0;
            }
            if (inputHistory_.empty())
                return 0;
            if (d->EventKey == ImGuiKey_UpArrow) {
                historyPos_ = historyPos_ < 0 ? static_cast<int>(inputHistory_.size()) - 1
                                              : std::max(0, historyPos_ - 1);
            } else if (historyPos_ >= 0) {
                ++historyPos_;
                if (historyPos_ >= static_cast<int>(inputHistory_.size()))
                    historyPos_ = -1;
            }
            const std::string& text = historyPos_ < 0
                                          ? std::string()
                                          : inputHistory_[static_cast<std::size_t>(historyPos_)];
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
        ImGui::TextDisabled("%zu lines. Ctrl+L or Esc closes.", lines.size());
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
        const float footer =
            ImGui::GetFrameHeightWithSpacing() + 2 +
            (pending && !pending->keywords.empty() ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
        ImGui::BeginChild("##history", ImVec2(0, -footer), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const auto& lines = core_.history();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lines.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                ImGui::TextUnformatted(lines[static_cast<std::size_t>(i)].c_str());
        if (lines.size() != lastHistorySize_) {
            ImGui::SetScrollHereY(1.0f);
            lastHistorySize_ = lines.size();
        }
        ImGui::EndChild();

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
        ImGui::TextUnformatted(prompt.c_str());
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(-1);
        if (refocusInput_) {
            ImGui::SetKeyboardFocusHere();
            refocusInput_ = false;
        }
        updateSuggestions();
        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter;
        if (ImGui::InputText("##command", &input_, flags, tbcad::app::inputCallback, this)) {
            std::string line = input_;
            if (suggestVisible_ && !suggestions_.empty())
                line = suggestions_[static_cast<std::size_t>(std::clamp(
                                        suggestSel_, 0, static_cast<int>(suggestions_.size()) - 1))]
                           .name;
            input_.clear();
            paletteOpen_ = false;
            if (!line.empty() && (inputHistory_.empty() || inputHistory_.back() != line))
                inputHistory_.push_back(line);
            historyPos_ = -1;
            core_.submitText(line);
            refocusInput_ = true;
            anyActivity_ = true;
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
        if (suggestVisible_)
            drawSuggestions(inputMin, inputW);
    }
    ImGui::End();
}

void Gui::updateSuggestions() {
    if (input_ != suggestFor_) {
        suggestFor_ = input_;
        suggestSel_ = 0;
        suggestDismissed_ = false;
    }
    const bool word = input_.find_first_of(" \t") == std::string::npos;
    suggestVisible_ = core_.promptInfo() == nullptr && !suggestDismissed_ && word &&
                      (paletteOpen_ || !input_.empty());
    if (!suggestVisible_) {
        suggestions_.clear();
        return;
    }
    suggestions_ = ui::matchCommands(core_.registry(), input_, paletteOpen_ ? 16 : 8);
    if (suggestions_.empty()) {
        suggestVisible_ = false;
        return;
    }
    suggestSel_ = std::clamp(suggestSel_, 0, static_cast<int>(suggestions_.size()) - 1);
}

void Gui::drawSuggestions(ImVec2 inputPos, float width) {
    ImGui::SetNextWindowPos(ImVec2(inputPos.x, inputPos.y - 4), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(width, 560.0f), 0), ImVec2(560, 400));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##suggest", nullptr, wf)) {
        const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const auto& keys = core_.keybindings();
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
                (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0))
                suggestSel_ = static_cast<int>(i);
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::SameLine(150);
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted(m.alias.empty() ? "" : ("(" + m.alias + ")").c_str());
            ImGui::SameLine(210);
            ImGui::TextUnformatted(m.description.c_str());
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
        if (i)
            ImGui::SameLine();
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
    ImGui::SetNextWindowPos(ImVec2(hudAnchor_.x + 16 * dpiScale_, hudAnchor_.y + 16 * dpiScale_));
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
        const cmd::FilterFeedback& fb = core_.feedback();
        if (fb.active && !fb.tooltip.empty()) {  // snap label, "distance < angle" tracking
            const Rgba snap = theme_.snap;
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(snap.r / 255.0f, snap.g / 255.0f, snap.b / 255.0f, 1.0f));
            ImGui::TextUnformatted(fb.tooltip.c_str());
            ImGui::PopStyleColor();
        }
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
        hudHovered_ = options && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    } else {
        hudHovered_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    anyActivity_ = true;
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
    if (!startPage_ || !core_.isPristine() || core_.documentCount() != 1)
        return;
    ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.45f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
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
        ImGui::BeginDisabled();
        ImGui::Button("Import DXF", ImVec2(110, 0));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("DXF import is not available yet.");

        const auto recent = core_.recentFiles();
        if (!recent.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Recent");
            for (const auto& path : recent) {
                const std::filesystem::path fp = platform::pathFromUtf8(path);
                ImGui::PushID(path.c_str());
                if (ImGui::Selectable(platform::pathToUtf8(fp.filename()).c_str(), false,
                                      ImGuiSelectableFlags_None, ImVec2(360, 0)))
                    doOpenPath(path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.c_str());
                ImGui::PopID();
            }
        }
        if (core_.settings().getBool("ui.gettingAround", true)) {
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

void Gui::quickProperties(const ViewportRect& rect) {
    if (core_.setting("ui.quickProps").value_or("0") != "1" || core_.selection().empty() ||
        core_.runner().active())
        return;
    const Document& doc = core_.document();
    std::vector<const Entity*> ents;
    for (EntityId id : core_.selection())
        if (const Entity* e = doc.find(id))
            ents.push_back(e);
    if (ents.empty())
        return;
    auto same = [&](auto get) {
        for (const Entity* e : ents)
            if (!(get(*e) == get(*ents.front())))
                return false;
        return true;
    };
    auto layerPath = [&](const Layer* l) {
        std::string path;
        for (; l; l = l->parent.value ? doc.layer(l->parent) : nullptr)
            path = l->name + (path.empty() ? "" : "/" + path);
        return path;
    };
    auto run = [&](const char* what, const std::string& value) {
        core_.submitText("CHPROP");
        core_.submitText(what);
        core_.submitText(value);
    };

    ImGui::SetNextWindowPos(ImVec2(rect.x + rect.w - 12, rect.y + 12), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##quickprops", nullptr, wf)) {
        if (ents.size() == 1)
            ImGui::TextUnformatted(kindName(kind(*ents.front())));
        else
            ImGui::Text("%zu objects", ents.size());
        ImGui::PushItemWidth(150.0f);

        const bool layerSame = same([](const Entity& e) { return e.common.layer; });
        const Layer* cur = layerSame ? doc.layer(ents.front()->common.layer) : nullptr;
        if (ImGui::BeginCombo("Layer", cur ? layerPath(cur).c_str() : "*VARIES*")) {
            for (const auto& [id, l] : doc.layers()) {
                const std::string path = layerPath(&l);
                if (ImGui::Selectable(path.c_str(), cur && cur->id == id))
                    run("Layer", path);
            }
            ImGui::EndCombo();
        }

        struct Named {
            const char* label;
            const char* value;
        };
        static const Named kColors[] = {
            {"ByLayer", "ByLayer"}, {"ByBlock", "ByBlock"}, {"1 Red", "1"},
            {"2 Yellow", "2"},      {"3 Green", "3"},       {"4 Cyan", "4"},
            {"5 Blue", "5"},        {"6 Magenta", "6"},     {"7 White", "7"}};
        std::string colorLabel = "*VARIES*";
        if (same([](const Entity& e) { return e.common.color; })) {
            const Color c = ents.front()->common.color;
            colorLabel = c.kind == ColorKind::ByLayer   ? "ByLayer"
                         : c.kind == ColorKind::ByBlock ? "ByBlock"
                         : c.kind == ColorKind::Index   ? std::to_string(int(c.r))
                                                        : "RGB";
            for (const Named& n : kColors)
                if (colorLabel == n.value)
                    colorLabel = n.label;
        }
        if (ImGui::BeginCombo("Color", colorLabel.c_str())) {
            for (const Named& n : kColors)
                if (ImGui::Selectable(n.label, colorLabel == n.label))
                    run("Color", n.value);
            ImGui::EndCombo();
        }

        std::string ltLabel = "*VARIES*";
        if (same([](const Entity& e) { return e.common.linetype; })) {
            const LinetypeId lt = ents.front()->common.linetype;
            const Linetype* row = doc.linetype(lt);
            ltLabel = lt == kLinetypeByLayer   ? "ByLayer"
                      : lt == kLinetypeByBlock ? "ByBlock"
                      : row                    ? row->name
                                               : "?";
        }
        if (ImGui::BeginCombo("Linetype", ltLabel.c_str())) {
            if (ImGui::Selectable("ByLayer", ltLabel == "ByLayer"))
                run("Linetype", "ByLayer");
            if (ImGui::Selectable("ByBlock", ltLabel == "ByBlock"))
                run("Linetype", "ByBlock");
            for (const auto& [id, l] : doc.linetypes())
                if (ImGui::Selectable(l.name.c_str(), ltLabel == l.name))
                    run("Linetype", l.name);
            ImGui::EndCombo();
        }

        static const Named kWeights[] = {
            {"ByLayer", "ByLayer"}, {"Default", "Default"}, {"0.13 mm", "0.13"},
            {"0.25 mm", "0.25"},    {"0.35 mm", "0.35"},    {"0.50 mm", "0.5"},
            {"0.70 mm", "0.7"},     {"1.00 mm", "1"},       {"2.00 mm", "2"}};
        std::string lwLabel = "*VARIES*";
        if (same([](const Entity& e) { return e.common.lineweight; })) {
            const LineWeight w = ents.front()->common.lineweight;
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f mm", w.v / 100.0);
            lwLabel = w.isByLayer()   ? "ByLayer"
                      : w.isByBlock() ? "ByBlock"
                      : w.isValue()   ? buf
                                      : "Default";
        }
        if (ImGui::BeginCombo("Lineweight", lwLabel.c_str())) {
            for (const Named& n : kWeights)
                if (ImGui::Selectable(n.label, lwLabel == n.label))
                    run("Lineweight", n.value);
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

void Gui::panels() {
    ui::PanelContext pc;
    pc.doc = &core_.document();
    pc.host = &core_;
    pc.selection = &core_.selection();
    pc.camera = &core_.camera();
    pc.runCommand = [this](const std::string& line) { core_.submitText(line); };
    pc.commandActive = core_.runner().active();
    pc.drawPropertySections = [this, &pc](EntityId id) {
        const Entity* e = core_.document().find(id);
        if (!e)
            return;
        for (const auto* section : registry_.sectionsFor(kind(*e))) {
            ImGui::Separator();
            section->draw(pc, id);
        }
    };
    for (const std::string& id : core_.takePanelRequests())
        panelOpen_[id] = true;
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
    for (const auto& p : registry_.panels()) {
        bool& open = panelOpen_[p.id];
        if (!open)
            continue;
        if (sideDock != 0) {
            ImGui::SetNextWindowDockID(sideDock, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
        }
        if (ImGui::Begin(p.title.c_str(), &open))
            p.draw(pc);
        ImGui::End();
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
    }
    if (leftDown_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!pointPrompt && !dragging_ &&
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
        } else {
            if (dragging_ && !lassoPath_.empty()) {
                core_.lassoSelect(lassoPath_, ps);
            } else if (dragging_) {
                core_.dragSelect(pressPos_, mouse, ps);
            } else if (io.KeyAlt) {
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

    // Right button: context menu, or Enter for users who set ui.rightClick = enter.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (core_.settings().getString("ui.rightClick", "menu") == "enter")
            core_.submitText("");
        else
            ImGui::OpenPopup("##viewportMenu");
    }
    viewportMenu();

    if (hovered || leftDown_ || panning_)
        core_.pointerMoved(ps);
    else
        core_.pointerLeft();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

// Content browser items and palette tools dragged onto the drawing run as if typed.
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
}

// What the right-click menu offers depends on the state: a running command (Enter, Cancel, its
// options), a selection (the common edits), or nothing (repeat, select all, undo, zoom).
void Gui::viewportMenu() {
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
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom Extents"))
            core_.zoomExtents();
    } else {
        const std::string last(core_.runner().lastCommand());
        if (!last.empty() && ImGui::MenuItem(("Repeat " + last).c_str()))
            run("");
        if (!core_.selection().empty()) {
            ImGui::Separator();
            for (const char* c : {"MOVE", "COPY", "ROTATE", "SCALE", "MIRROR", "ERASE"})
                if (core_.registry().find(c) && ImGui::MenuItem(c))
                    run(c);
            if (ImGui::MenuItem("Deselect All"))
                core_.clearSelection();
        } else if (ImGui::MenuItem("Select All")) {
            core_.selectAll();
        }
        ImGui::Separator();
        const std::string undo = core_.undoLabel();
        const std::string redo = core_.redoLabel();
        if (ImGui::MenuItem(undo.empty() ? "Undo" : undo.c_str(), nullptr, false, !undo.empty()))
            core_.undo();
        if (ImGui::MenuItem(redo.empty() ? "Redo" : redo.c_str(), nullptr, false, !redo.empty()))
            core_.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom Extents"))
            core_.zoomExtents();
    }
    ImGui::EndPopup();
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
        else if (what.rfind("workspace:load:", 0) == 0)
            switchWorkspace(what.substr(15));
        else if (what.rfind("workspace:save:", 0) == 0)
            saveWorkspace(what.substr(15));
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
        statusBar();
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(kDockId, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    dockLayout(kDockId);
    commandWindow();
    if (!cleanScreen_) {
        panels();
    }
    logWindow();

    ViewportRect rect{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
    if (ImGuiDockNode* center = ImGui::DockBuilderGetCentralNode(kDockId)) {
        rect = {center->Pos.x, center->Pos.y, center->Size.x, center->Size.y};
    }
    const float s = ImGui::GetIO().DisplayFramebufferScale.x;
    core_.setViewportSize(static_cast<int>(std::lround(rect.w * s)),
                          static_cast<int>(std::lround(rect.h * s)));
    viewportInput(rect);
    dropIntoViewport(rect);
    lastRect_ = rect;
    startPage(rect);
    cursorHud(rect);
    quickProperties(rect);

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
    return rect;
}

void Gui::showViewport(unsigned texture, const ViewportRect& rect) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    bg->AddImage(static_cast<ImTextureID>(texture), ImVec2(rect.x, rect.y),
                 ImVec2(rect.x + rect.w, rect.y + rect.h), ImVec2(0, 1), ImVec2(1, 0));
}

// ---- dialogs -----------------------------------------------------------------------------------

void Gui::dialogs() {
    settings_.draw(core_);
    workspaceDialog();
    if (openRecoveryPrompt_) {
        ImGui::OpenPopup("Recover drawing?");
        openRecoveryPrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Recover drawing?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TBCAD did not close normally last time.");
        if (recoveryCount_ > 1)
            ImGui::Text("Automatic backups of %zu unsaved drawings were found.", recoveryCount_);
        else if (recovery_)
            ImGui::TextUnformatted("An automatic backup of an unsaved drawing was found.");
        if (recovery_ && !recovery_->originalPath.empty())
            ImGui::Text("Original file: %s", recovery_->originalPath.c_str());
        if (!crashReport_.empty()) {
            ImGui::TextWrapped(
                "A crash report was saved to %s. Attach it when you report the problem.",
                crashReport_.c_str());
            if (!recovery_) {
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(110, 0))) {
                    platform::acknowledgeCrashReports(core_.paths().crashDir());
                    crashReport_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::Spacing();
        if (recovery_ && ImGui::Button("Recover", ImVec2(110, 0))) {
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
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
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
        if (ImGui::Button("OK", ImVec2(110, 0)))
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
        ImGui::BulletText("Lato and Sarabun fonts (SIL Open Font License)");
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
        ImGui::TextUnformatted(quickStartText_.c_str());
        ImGui::PopTextWrapPos();
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
            } else if (browserPurpose_ == BrowserPurpose::ExportDxf) {
                dxfCommandWithPath("EXPORTDXF", *path);
            } else {
                auto r = core_.saveAs(*path);
                if (!r)
                    lastError_ = r.error().message;
                else if (saveThenPending_)
                    performPending(false);
                saveThenPending_ = false;
            }
        }
    }
}

}  // namespace tbcad::app
