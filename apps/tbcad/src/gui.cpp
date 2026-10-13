#include "gui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include "key_map.h"
#include "tbcad/platform/file_dialog.h"
#include "tbcad/platform/paths.h"
#include "tbcad/ui/selection.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace tbcad::app {

namespace {

constexpr ImGuiID kDockId = 0x7BC4D001u;

const std::vector<platform::FileFilter>& drawingFilters() {
    static const std::vector<platform::FileFilter> f = {{"TBCAD drawings", "*.tbcad"}};
    return f;
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
    : core_(core), registry_(registry), window_(window) {
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    dpiScale_ = std::max(1.0f, sx);
    recovery_ = core_.pendingRecovery();
    openRecoveryPrompt_ = recovery_.has_value();
    anyActivity_ = true;
    dark_ = core_.settings().getString("ui.theme", "dark") != "light";
    lineweights_ = core_.settings().getBool("view.lineweights", true);
    for (const auto& p : registry_.panels())
        panelOpen_[p.id] = p.visibleByDefault;
    installImeHook(window);
}

void Gui::setTheme(bool dark) {
    dark_ = dark;
    core_.settings().setString("ui.theme", dark ? "dark" : "light");
    applyTheme(dark, dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0)));
}

void Gui::setDpiScale(float scale) {
    dpiScale_ = std::max(0.5f, scale);
    const float total = dpiScale_ * static_cast<float>(core_.settings().getNumber("ui.scale", 1.0));
    buildFonts(total);
    applyTheme(dark_, total);
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
    if (action == "file.new")
        requestAction(Pending::New);
    else if (action == "file.open")
        requestAction(Pending::Open);
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
    else if (action == "edit.deselect") {
        core_.pressEscape();
        input_.clear();
    } else if (action == "view.zoomExtents")
        core_.zoomExtents();
    else if (action == "view.toggleCommandLine")
        showCommandLine_ = !showCommandLine_;
    else {
        for (const auto& item : registry_.menuItems())
            if (item.actionId == action) {
                core_.submitText(item.command);
                break;
            }
    }
    anyActivity_ = true;
}

void Gui::requestAction(Pending what, std::string path) {
    pending_ = what;
    pendingPath_ = std::move(path);
    if (core_.modified())
        openSavePrompt_ = true;
    else
        performPending();
}

void Gui::performPending() {
    Pending what = pending_;
    pending_ = Pending::None;
    switch (what) {
        case Pending::New:
            core_.newDocument();
            break;
        case Pending::Open:
            doOpen();
            break;
        case Pending::OpenRecent:
            doOpenPath(pendingPath_);
            break;
        case Pending::Exit:
            exitRequested_ = true;
            break;
        case Pending::None:
            break;
    }
}

bool Gui::requestClose() {
    if (!core_.modified())
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
    browser_.open(FileBrowser::Mode::Open, start, "", ".tbcad");
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
    std::string suggested = core_.hasFile() ? core_.file().filename().string() : "Drawing1.tbcad";
    if (platform::nativeDialogsAvailable()) {
        if (auto p = platform::showSaveDialog(nativeHandle(), drawingFilters(), "Save drawing as",
                                              "tbcad", suggested)) {
            auto r = core_.saveAs(*p);
            if (!r)
                lastError_ = r.error().message;
            else if (saveThenPending_)
                performPending();
        }
        saveThenPending_ = false;
        return;
    }
    std::filesystem::path start =
        core_.hasFile() ? core_.file().parent_path() : std::filesystem::path{};
    browserPurpose_ = BrowserPurpose::SaveAs;
    browser_.open(FileBrowser::Mode::Save, start, suggested, ".tbcad");
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
        if (io.WantTextInput && !chord->ctrl && !chord->alt && chord->key != "ESCAPE" &&
            chord->key[0] != 'F')
            continue;
        if (ImGui::IsKeyChordPressed(*ic)) {
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
                    requestAction(Pending::OpenRecent, path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", keys.label("file.save").c_str()))
            runAction("file.save");
        if (ImGui::MenuItem("Save As...", keys.label("file.saveAs").c_str()))
            runAction("file.saveAs");
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
        for (const auto& p : registry_.panels())
            ImGui::MenuItem(p.title.c_str(), nullptr, &panelOpen_[p.id]);
        ImGui::Separator();
        if (ImGui::MenuItem("Dark Theme", nullptr, dark_))
            setTheme(true);
        if (ImGui::MenuItem("Light Theme", nullptr, !dark_))
            setTheme(false);
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

void Gui::commandWindow() {
    if (!showCommandLine_)
        return;
    ImGui::SetNextWindowSize(ImVec2(700, 160), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Command", &showCommandLine_)) {
        const float footer = ImGui::GetFrameHeightWithSpacing() + 2;
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

        ImGui::AlignTextToFramePadding();
        std::string prompt = core_.promptText();
        ImGui::TextUnformatted(prompt.c_str());
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(-1);
        if (refocusInput_) {
            ImGui::SetKeyboardFocusHere();
            refocusInput_ = false;
        }
        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter;
        if (ImGui::InputText("##command", &input_, flags, tbcad::app::inputCallback, this)) {
            std::string line = input_;
            input_.clear();
            if (!line.empty() && (inputHistory_.empty() || inputHistory_.back() != line))
                inputHistory_.push_back(line);
            historyPos_ = -1;
            core_.submitText(line);
            refocusInput_ = true;
            anyActivity_ = true;
        }
        inputId_ = ImGui::GetItemID();
        if (!typedWhileUnfocused_.empty() && ImGui::IsItemActive()) {
            ImGui::GetIO().AddInputCharactersUTF8(typedWhileUnfocused_.c_str());
            typedWhileUnfocused_.clear();
        }
        if (spaceAsEnter_) {
            spaceAsEnter_ = false;
            core_.submitText("");
            refocusInput_ = true;
        }
    }
    ImGui::End();
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

ViewportRect Gui::frame() {
    shortcuts();
    menuBar();
    toolStrip();
    statusBar();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(kDockId, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    dockLayout(kDockId);
    commandWindow();
    propertiesWindow();
    panels();

    ViewportRect rect{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
    if (ImGuiDockNode* center = ImGui::DockBuilderGetCentralNode(kDockId)) {
        rect = {center->Pos.x, center->Pos.y, center->Size.x, center->Size.y};
    }
    const float s = ImGui::GetIO().DisplayFramebufferScale.x;
    core_.setViewportSize(static_cast<int>(std::lround(rect.w * s)),
                          static_cast<int>(std::lround(rect.h * s)));
    viewportInput(rect);
    lastRect_ = rect;

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
                    performPending();
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
            performPending();
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
            } else {
                auto r = core_.saveAs(*path);
                if (!r)
                    lastError_ = r.error().message;
                else if (saveThenPending_)
                    performPending();
                saveThenPending_ = false;
            }
        }
    }
}

}  // namespace tbcad::app
