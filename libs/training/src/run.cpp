#include "tbcad/training/run.h"

#include <algorithm>

#include "tbcad/platform/json.h"

namespace tbcad::training {

int starsFor(const Lesson& lesson, bool done, bool hintUsed, int commands, double seconds) {
    if (!done)
        return 0;
    if (hintUsed)
        return 1;
    const bool inTime = lesson.parSeconds <= 0 || seconds <= lesson.parSeconds;
    return commands <= lesson.par && inTime ? 3 : 2;
}

bool countsTowardsPar(std::string_view command) {
    static constexpr std::string_view kFree[] = {"ZOOM",  "PAN",   "UNDO",     "U",    "REDO",
                                                 "MREDO", "REGEN", "REGENALL", "HELP", "PRACTICE"};
    return std::find(std::begin(kFree), std::end(kFree), command) == std::end(kFree);
}

LessonRun::LessonRun(const Lesson& lesson) : lesson_(lesson), done_(lesson.steps.size(), false) {}

void LessonRun::record(const Event& e) {
    events_.push_back(e);
    if (e.kind == Event::Kind::Undo ||
        (e.kind == Event::Kind::Started && (e.command == "UNDO" || e.command == "U")))
        ++undos_;
    if (e.kind == Event::Kind::Started && !e.transparent && countsTowardsPar(e.command))
        ++commands_;
}

bool LessonRun::update(Facts facts) {
    facts.events = events_;
    bool changed = false;
    for (std::size_t i = done_.size(); i-- > 0;) {
        if (done_[i])
            break;  // the steps before a done step are done too
        const auto& checks = lesson_.steps[i].checks;
        if (checks.empty())
            continue;
        if (std::all_of(checks.begin(), checks.end(),
                        [&](const Check& c) { return passes(c, facts); })) {
            std::fill(done_.begin(), done_.begin() + static_cast<std::ptrdiff_t>(i) + 1, true);
            changed = true;
            break;
        }
    }
    return changed;
}

std::size_t LessonRun::currentStep() const {
    const auto it = std::find(done_.begin(), done_.end(), false);
    return static_cast<std::size_t>(it - done_.begin());
}

Score LessonRun::score(double seconds) const {
    Score s;
    s.seconds = seconds;
    s.commands = commands_;
    s.undos = undos_;
    s.hintUsed = hintUsed_;
    s.stars = starsFor(lesson_, complete(), hintUsed_, commands_, seconds);
    return s;
}

bool Progress::record(const std::string& lessonId, const Score& score) {
    if (score.stars <= 0)
        return false;
    LessonRecord& r = lessons[lessonId];
    bool better = false;
    if (score.stars > r.stars) {
        r.stars = score.stars;
        better = true;
    }
    if (r.bestSeconds <= 0 || score.seconds < r.bestSeconds) {
        r.bestSeconds = score.seconds;
        better = true;
    }
    return better;
}

int Progress::stars(const std::string& lessonId) const {
    const auto it = lessons.find(lessonId);
    return it == lessons.end() ? 0 : it->second.stars;
}

std::string Progress::toJson() const {
    platform::Json::Object ls;
    for (const auto& [id, r] : lessons)
        ls[id] = platform::Json::Object{{"stars", r.stars}, {"best", r.bestSeconds}};
    platform::Json::Array bs;
    for (const std::string& b : badges)
        bs.emplace_back(b);
    return platform::toJson(platform::Json::Object{{"lessons", ls}, {"badges", bs}}, false);
}

Progress Progress::fromJson(std::string_view text) {
    Progress p;
    auto parsed = platform::parseJson(text);
    if (!parsed)
        return p;
    const platform::Json& root = parsed.value();
    if (const platform::Json* ls = root.find("lessons"))
        for (const auto& [id, r] : ls->asObject()) {
            LessonRecord rec;
            if (const platform::Json* s = r.find("stars"))
                rec.stars = std::clamp(static_cast<int>(s->asNumber()), 0, 3);
            if (const platform::Json* b = r.find("best"))
                rec.bestSeconds = std::max(b->asNumber(), 0.0);
            p.lessons[id] = rec;
        }
    if (const platform::Json* bs = root.find("badges"))
        for (const platform::Json& b : bs->asArray())
            if (b.isString())
                p.badges.insert(b.asString());
    return p;
}

}  // namespace tbcad::training
