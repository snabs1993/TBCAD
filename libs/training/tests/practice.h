#pragma once

#include <doctest/doctest.h>

#include <cstdio>
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
        core->applyUiKeybindings(uiRegistry);  // snaps, grips and the like, as the window has
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
        core->newScratchDocument("Practice " + lesson.id);
        core->runSetup(lesson.setup);
        run = std::make_unique<LessonRun>(lesson);
        run->update(facts());
    }

    // Like AppCore::submitScript, with the steps checked after every part. A few parts stand for
    // what is not typed: "!LINE" picks LINE from a button, "<Esc>" presses Esc, "<Click x y>"
    // clicks a drawing point and "<Drag x0 y0 x1 y1>" drags a selection box between two.
    void type(const std::string& script) {
        for (std::size_t at = 0; at <= script.size();) {
            const std::size_t end = script.find(';', at);
            const std::string part = script.substr(at, end == std::string::npos ? end : end - at);
            if (part.size() > 1 && part[0] == '!') {
                core->startCommand(part.substr(1));
            } else if (part == "<Esc>") {
                core->pressEscape();
            } else if (part.rfind("<Click ", 0) == 0) {
                double x = 0, y = 0;
                REQUIRE(std::sscanf(part.c_str(), "<Click %lf %lf>", &x, &y) == 2);
                ui::PointerState ps;
                ps.screen = core->camera().worldToScreen({x, y});
                core->pointerMoved(ps);
                if (!core->pressOnGrip(ps))  // as the window does
                    core->click(ps);
            } else if (part.rfind("<Drag ", 0) == 0) {
                double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                REQUIRE(std::sscanf(part.c_str(), "<Drag %lf %lf %lf %lf>", &x0, &y0, &x1, &y1) ==
                        4);
                const render::Camera2D& cam = core->camera();
                core->dragSelect(cam.worldToScreen({x0, y0}), cam.worldToScreen({x1, y1}), {});
            } else {
                core->submitText(part);
            }
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
