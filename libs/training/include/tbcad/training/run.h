#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/training/check.h"
#include "tbcad/training/lesson.h"

namespace tbcad::training {

struct Score {
    int stars = 0;  // 0 while the lesson is not done
    double seconds = 0;
    int commands = 0;
    int undos = 0;
    bool hintUsed = false;
};

// One star for finishing, one for no hint, one for no hint within par (and, for a challenge, within
// its time).
int starsFor(const Lesson& lesson, bool done, bool hintUsed, int commands, double seconds);

// Commands that move the view or the history: not counted against par.
bool countsTowardsPar(std::string_view command);

// One attempt at a lesson: collects what the user does and ticks the steps off.
class LessonRun {
public:
    explicit LessonRun(const Lesson& lesson);

    const Lesson& lesson() const { return lesson_; }
    void record(const Event& e);
    void useHint() { hintUsed_ = true; }
    bool hintUsed() const { return hintUsed_; }

    // Checks the steps not done yet against the drawing. A step that passes is done for good, and
    // so are the steps before it. True when a step was newly done.
    bool update(Facts facts);

    bool stepDone(std::size_t i) const { return i < done_.size() && done_[i]; }
    // The first step not done; steps().size() when the lesson is done.
    std::size_t currentStep() const;
    bool complete() const { return currentStep() == lesson_.steps.size(); }
    int commandsUsed() const { return commands_; }
    int undos() const { return undos_; }
    const std::vector<Event>& events() const { return events_; }
    Score score(double seconds) const;

private:
    const Lesson& lesson_;
    std::vector<bool> done_;
    std::vector<Event> events_;
    int commands_ = 0;
    int undos_ = 0;
    bool hintUsed_ = false;
};

// The user's results, kept in the settings as JSON.
struct LessonRecord {
    int stars = 0;
    double bestSeconds = 0;  // 0 = not done
};

struct Progress {
    std::map<std::string, LessonRecord> lessons{};
    std::set<std::string> badges{};

    // Keeps the better stars and the better time. True when something improved.
    bool record(const std::string& lessonId, const Score& score);
    int stars(const std::string& lessonId) const;

    std::string toJson() const;
    // Unreadable text gives empty progress.
    static Progress fromJson(std::string_view text);
};

}  // namespace tbcad::training
