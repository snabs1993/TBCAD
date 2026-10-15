#include "file_browser.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace tbcad::app {

namespace fs = std::filesystem;

void FileBrowser::open(Mode mode, fs::path startDir, std::string suggestedName,
                       std::string extension) {
    mode_ = mode;
    std::error_code ec;
    if (startDir.empty() || !fs::is_directory(startDir, ec))
        startDir = fs::current_path(ec);
    dir_ = fs::weakly_canonical(startDir, ec);
    if (ec)
        dir_ = startDir;
    name_ = std::move(suggestedName);
    extension_ = std::move(extension);
    requestOpen_ = true;
    open_ = true;
    refresh();
}

// extension_ holds one or more extensions separated by ';' (".tbcad;.dxf"), compared ignoring case.
bool FileBrowser::extensionMatches(const fs::path& p) const {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t start = 0;
    while (start <= extension_.size()) {
        std::size_t end = extension_.find(';', start);
        if (end == std::string::npos)
            end = extension_.size();
        if (extension_.substr(start, end - start) == e)
            return true;
        start = end + 1;
    }
    return false;
}

void FileBrowser::refresh() {
    items_.clear();
    error_.clear();
    dirText_ = dir_.string();
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        bool isDir = it->is_directory(e2);
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.')
            continue;
        if (!isDir && !extension_.empty() && !extensionMatches(it->path()))
            continue;
        items_.push_back({name, isDir});
    }
    if (ec)
        error_ = "Cannot read this folder: " + ec.message();
    std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name < b.name;
    });
}

std::optional<fs::path> FileBrowser::draw() {
    if (!open_)
        return std::nullopt;
    if (requestOpen_) {
        ImGui::OpenPopup("File");
        requestOpen_ = false;
    }
    std::optional<fs::path> result;
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);
    bool keepOpen = true;
    if (ImGui::BeginPopupModal("File", &keepOpen)) {
        ImGui::TextUnformatted(mode_ == Mode::Open ? "Open drawing" : "Save drawing as");
        if (ImGui::InputText("Folder", &dirText_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::error_code ec;
            if (fs::is_directory(dirText_, ec)) {
                dir_ = dirText_;
                refresh();
            }
        }
        if (ImGui::Button("Up") && dir_.has_parent_path() && dir_.parent_path() != dir_) {
            dir_ = dir_.parent_path();
            refresh();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", error_.c_str());
        ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true);
        for (const Item& item : items_) {
            std::string label = (item.isDir ? "[folder] " : "") + item.name;
            if (ImGui::Selectable(label.c_str(), !item.isDir && item.name == name_,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (item.isDir) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        dir_ /= item.name;
                        refresh();
                        break;
                    }
                } else {
                    name_ = item.name;
                    if (ImGui::IsMouseDoubleClicked(0) && mode_ == Mode::Open)
                        result = dir_ / name_;
                }
            }
        }
        ImGui::EndChild();
        if (mode_ == Mode::Save && ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();  // type the name straight away
        ImGui::InputText("File name", &name_);
        ImGui::SameLine();
        if (ImGui::Button(mode_ == Mode::Open ? "Open" : "Save") && !name_.empty())
            result = dir_ / name_;
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            keepOpen = false;
        if (result || !keepOpen) {
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::EndPopup();
    } else if (!keepOpen) {
        open_ = false;
    }
    return result;
}

}  // namespace tbcad::app
