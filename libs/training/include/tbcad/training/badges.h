#pragma once

#include <string>
#include <vector>

#include "tbcad/training/run.h"

namespace tbcad::training {

// A badge (docs/training.md): earned once, kept in Progress::badges.
struct Badge {
    std::string id;
    Text name;
    Text how;  // what earns it
};

// Every badge, in the order the lesson map shows them.
const std::vector<Badge>& allBadges();
const Badge* findBadge(const std::string& id);

// Call when `run` has just completed `lesson` (Progress::record already done). Updates the counts
// some badges keep across lessons and adds the badges now earned to progress.badges; returns
// their ids. `lessons` is every lesson (for "finish a whole world"); `facts` is the finished
// drawing.
std::vector<std::string> awardBadges(Progress& progress, const std::vector<Lesson>& lessons,
                                     const Lesson& lesson, const LessonRun& run,
                                     const Facts& facts);

}  // namespace tbcad::training
