#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace tbcad::app {

// A small Dear ImGui file picker, used where the platform has no native dialog (anything but
// Windows). Modal; call draw() every frame while open().
class FileBrowser {
public:
    enum class Mode { Open, Save };

    void open(Mode mode, std::filesystem::path startDir, std::string suggestedName,
              std::string extension);
    bool isOpen() const { return open_; }
    // Returns the chosen path in the frame the user confirms, otherwise nullopt.
    std::optional<std::filesystem::path> draw();

private:
    void refresh();

    bool open_ = false;
    bool requestOpen_ = false;
    Mode mode_ = Mode::Open;
    std::filesystem::path dir_{};
    std::string name_{};
    bool matchesExtension(const std::filesystem::path& p) const;
    std::string extension_{};
    std::string dirText_{};
    struct Item {
        std::string name;
        bool isDir;
    };
    std::vector<Item> items_{};
    std::string error_{};
};

}  // namespace tbcad::app
