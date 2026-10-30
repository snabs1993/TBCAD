#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/training/run.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::app {

// Practice mode (Help > Practice Mode, PRACTICE): the "Practice" panel with the lesson map and the
// steps of the lesson being played, the ghost targets on the canvas, and the saved progress.
// Each lesson is played in a practice drawing of its own (AppCore::newScratchDocument); the
// lessons are assets/training/lessons/*.json (docs/training.md).
class Practice {
public:
    Practice(ui::AppCore& core, ui::UiRegistry& registry, const std::filesystem::path& lessonDir);

    // Shows the panel.
    void open();
    // Starts lessons()[index] in a new practice drawing.
    void start(std::size_t index);
    // Checks the steps against the practice drawing; called once per frame.
    void tick();

    const std::vector<training::Lesson>& lessons() const { return lessons_; }
    const training::LessonRun* run() const { return run_.get(); }
    bool finished() const { return finished_; }
    const training::Progress& progress() const { return progress_; }

private:
    void draw(ui::PanelContext& pc);
    void drawMap();
    void drawLesson();
    void drawResult();
    void drawGhosts(render::Overlay& overlay) const;
    bool inPracticeDrawing() const;
    bool practiceDrawingOpen() const;
    void leave();
    training::Facts facts() const;
    double elapsed() const;
    void saveProgress();

    ui::AppCore& core_;
    std::vector<training::Lesson> lessons_;
    std::vector<std::string> loadErrors_;
    training::Progress progress_;
    std::optional<std::size_t> active_;
    std::unique_ptr<training::LessonRun> run_;
    std::uint64_t docId_ = 0;
    std::chrono::steady_clock::time_point startedAt_{};
    bool finished_ = false;
    bool showHint_ = false;
    training::Score score_{};
    bool improved_ = false;
};

}  // namespace tbcad::app
