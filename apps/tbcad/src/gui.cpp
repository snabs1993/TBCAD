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

#include "key_map.h"
#include "tbcad/platform/file_dialog.h"
#include "tbcad/platform/paths.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/selection.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace tbcad::app {

namespace {

constexpr ImGuiID kDockId = 0x7BC4D001u;

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
          [this](const std::string& action) { return actionLabel(action); }}) {
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    dpiScale_ = std::max(1.0f, sx);
    recovery_ = core_.pendingRecovery();
    openRecoveryPrompt_ = recovery_.has_value();
    anyActivity_ = true;
    themeMode_ = ui::parseThemeMode(core_.settings().getString("ui.theme", "dark"));
    theme_ = ui::themeFor(themeMode_, systemPrefersDark());
    core_.setTheme(theme_);
    lineweights_ = core_.settings().getBool("view.lineweights", true);
    cursorHud_ = core_.settings().getBool("hud.enabled", true);
    for (const auto& p : registry_.panels())
        panelOpen_[p.id] = p.visibleByDefault;
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
        if (auto p = platform::showOpenDialog(nativeHandle(), drawingFilters(), "Open drawing"))
            doOpenPath(platform::pathToUtf8(*p));
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::Open;
    browser_.open(FileBrowser::Mode::Open, start, "", ".tbcad;.dxf");
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
    if (!core_.hasFile()) {
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
        ImGui::MenuItem("Properties", nullptr, &showProperties_);
        if (ImGui::MenuItem("Command Log", keys.label("view.expandLog").c_str(), logExpanded_))
            logExpanded_ = !logExpanded_;
        if (ImGui::MenuItem("Clean Screen", keys.label("view.cleanScreen").c_str(), cleanScreen_))
            cleanScreen_ = !cleanScreen_;
        for (const auto& p : registry_.panels())
            ImGui::MenuItem(p.title.c_str(), nullptr, &panelOpen_[p.id]);
        ImGui::Separator();
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
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking;
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 1.2f;
    if (ImGui::BeginViewportSideBar("##toolstrip", vp, ImGuiDir_Up, h, flags)) {
        auto button = [&](const char* label, bool enabled = true) {
            ImGui::BeginDisabled(!enabled);
            bool pressed = ImGui::Button(label);
            ImGui::EndDisabled();
            ImGui::SameLine();
            return pressed;
        };
        if (button("New"))
            runAction("file.new");
        if (button("Open"))
            runAction("file.open");
        if (button("Save"))
            runAction("file.save");
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (button("Undo", !core_.undoLabel().empty()))
            core_.undo();
        if (button("Redo", !core_.redoLabel().empty()))
            core_.redo();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (button("Zoom Extents"))
            core_.zoomExtents();
        ImGui::NewLine();
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
    if (existing && (existing->IsSplitNode() || existing->Windows.Size > 0))
        return;  // restored from imgui.ini
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(ds);
    ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ds, vp->WorkSize);
    ImGuiID center = ds;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Command", bottom);
    ImGui::DockBuilderDockWindow("Properties", right);
    for (const auto& p : registry_.panels())
        ImGui::DockBuilderDockWindow(p.title.c_str(), right);
    ImGui::DockBuilderFinish(ds);
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
            if (info->defaultText)
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
    if (!options)
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

void Gui::propertiesWindow() {
    if (!showProperties_)
        return;
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Properties", &showProperties_)) {
        const auto& sel = core_.selection();
        const Document& doc = core_.document();
        if (sel.empty()) {
            ImGui::TextDisabled("Nothing selected");
        } else if (sel.size() == 1) {
            const Entity* e = doc.find(sel[0]);
            if (e) {
                ImGui::TextUnformatted(kindName(kind(*e)));
                const Layer* layer = doc.layer(e->common.layer);
                ImGui::Text("Layer: %s", layer ? layer->name.c_str() : "?");
                ui::PanelContext pc;
                pc.doc = &core_.document();
                pc.host = &core_;
                pc.selection = &sel;
                pc.camera = &core_.camera();
                pc.runCommand = [this](const std::string& line) { core_.submitText(line); };
                for (const auto* section : registry_.sectionsFor(kind(*e))) {
                    ImGui::Separator();
                    section->draw(pc, sel[0]);
                }
            }
        } else {
            std::map<std::string, int> counts;
            for (EntityId id : sel)
                if (const Entity* e = doc.find(id))
                    ++counts[kindName(kind(*e))];
            ImGui::Text("%zu objects selected", sel.size());
            for (const auto& [name, n] : counts)
                ImGui::BulletText("%s: %d", name.c_str(), n);
        }
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
    for (const std::string& id : core_.takePanelRequests())
        panelOpen_[id] = true;
    for (const auto& p : registry_.panels()) {
        bool& open = panelOpen_[p.id];
        if (!open)
            continue;
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
            if (dragging_)
                core_.setSelectionDrag(std::make_pair(pressPos_, mouse));
        } else {
            if (dragging_)
                core_.dragSelect(pressPos_, mouse, ps);
            else if (doubleClick_)
                core_.doubleClick(ps);
            else
                core_.click(ps);
            core_.setSelectionDrag(std::nullopt);
            leftDown_ = dragging_ = false;
        }
    }

    if (hovered || leftDown_ || panning_)
        core_.pointerMoved(ps);
    else
        core_.pointerLeft();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
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
    }
    if (!cleanScreen_) {  // F11 hides everything but the viewport and the command line
        menuBar();
        toolStrip();
        docTabs();
        statusBar();
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(kDockId, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    dockLayout(kDockId);
    commandWindow();
    if (!cleanScreen_) {
        propertiesWindow();
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
    lastRect_ = rect;
    startPage(rect);
    cursorHud(rect);

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
    if (openRecoveryPrompt_) {
        ImGui::OpenPopup("Recover drawing?");
        openRecoveryPrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Recover drawing?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TBCAD did not close normally last time.");
        ImGui::TextUnformatted("An automatic backup of an unsaved drawing was found.");
        if (recovery_ && !recovery_->originalPath.empty())
            ImGui::Text("Original file: %s", recovery_->originalPath.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Recover", ImVec2(110, 0))) {
            if (recovery_) {
                auto r = core_.recover(*recovery_);
                if (!r)
                    lastError_ = r.error().message;
            }
            recovery_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
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
