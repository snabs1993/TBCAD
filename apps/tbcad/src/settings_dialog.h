#pragma once

#include <functional>
#include <optional>
#include <string>

#include "tbcad/ui/app_core.h"
#include "tbcad/ui/settings_model.h"

namespace tbcad::app {

// The Settings dialog (Tools > Settings): General, Display, Drafting, Files, Aliases and
// Keybindings pages. Edits to aliases and key bindings are validated and written to
// aliases.json / keybindings.json at once; the other pages change settings.json values.
class SettingsDialog {
public:
    struct Hooks {
        std::function<int()> themeMode{};  // 0 dark, 1 light, 2 follow Windows
        std::function<void(int)> setThemeMode{};
        std::function<void()> scaleChanged{};
        // Label for a key binding action ("file.save" -> "File > Save"); falls back to the id.
        std::function<std::string(const std::string&)> actionLabel{};
    };

    explicit SettingsDialog(Hooks hooks) : hooks_(std::move(hooks)) {}

    // `page` (General, Display, Drafting, Files, Aliases, Keybindings) selects a tab; empty keeps
    // it.
    void open(const std::string& page = {}) {
        wantOpen_ = true;
        selectPage_ = page;
    }
    bool isOpen() const { return open_; }
    // Call every frame; draws nothing while closed.
    void draw(ui::AppCore& core);

private:
    void general(ui::AppCore& core);
    void display(ui::AppCore& core);
    void drafting(ui::AppCore& core);
    void files(ui::AppCore& core);
    void aliases(ui::AppCore& core);
    void keybindings(ui::AppCore& core);
    void captureKey(ui::AppCore& core);

    Hooks hooks_;
    bool wantOpen_ = false;
    bool open_ = false;
    std::string selectPage_{};

    // Aliases page.
    std::string newAlias_{};
    std::string newCommand_{};
    std::string aliasFilter_{};
    std::string aliasError_{};
    // Keybindings page.
    std::string keyFilter_{};
    std::string capturing_{};  // action being rebound; empty = none
    std::string keyMessage_{};
    std::string pendingChord_{};
    std::string pendingConflict_{};
};

}  // namespace tbcad::app
