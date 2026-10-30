#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "tbcad/cmd/register_all.h"
#include "tbcad/training/run.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::training::test {

// The real program without a window, with a lesson attempt listening to it.
struct Practice {
    std::filesystem::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;
    std::unique_ptr<LessonRun> run;

    explicit Practice(const std::string& tag) {
        dir = std::filesystem::temp_directory_path() / ("tbcad_training_" + tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->setViewportSize(1000, 800);
        core->addCommandListener([this](const ui::CommandEvent& e) {
            if (!run)
                return;
            Event ev;
            ev.kind = static_cast<Event::Kind>(e.kind);
            ev.command = e.command;
            ev.source = classifySource(e.word, e.fromButton, e.command);
            ev.transparent = e.transparent;
            ev.cancelled = e.cancelled;
            run->record(ev);
        });
    }
    ~Practice() {
        run.reset();
        core.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Draws the starting drawing, then starts listening.
    void start(const Lesson& lesson) {
        for (const std::string& line : lesson.setup)
            core->submitScript(line);
        run = std::make_unique<LessonRun>(lesson);
        run->update(facts());
    }

    // Like AppCore::submitScript, with the steps checked after every part.
    void type(const std::string& script) {
        for (std::size_t at = 0; at <= script.size();) {
            const std::size_t end = script.find(';', at);
            core->submitText(script.substr(at, end == std::string::npos ? end : end - at));
            run->update(facts());
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
    }

    Facts facts() const {
        Facts f;
        f.doc = &core->document();
        f.selection = core->selection();
        const cmd::ViewState v = core->view();
        BBox2 box;
        box.expand({v.center.x - v.sizePx.x * v.scale / 2, v.center.y - v.sizePx.y * v.scale / 2});
        box.expand({v.center.x + v.sizePx.x * v.scale / 2, v.center.y + v.sizePx.y * v.scale / 2});
        f.view = box;
        if (const cmd::PromptInfo* p = core->promptInfo()) {
            f.runningCommand = p->command;
            f.prompt = p->text;
        }
        return f;
    }
};

}  // namespace tbcad::training::test
