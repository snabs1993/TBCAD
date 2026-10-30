// Every lesson in assets/training/lessons is played with the real commands: the starting drawing
// must not pass already, the lesson's solution must pass within par, and its wrong attempts must
// not pass. A command that changes how it behaves breaks the lessons that teach it here first.
#include <doctest/doctest.h>

#include <algorithm>
#include <set>

#include "practice.h"

using namespace tbcad;
using namespace tbcad::training;
using namespace tbcad::training::test;

namespace {

std::vector<Lesson> lessons() {
    std::vector<std::string> errors;
    std::vector<Lesson> out = loadLessons(TBCAD_LESSON_DIR, &errors);
    for (const std::string& e : errors)
        FAIL_CHECK(e);
    return out;
}

}  // namespace

TEST_CASE("every lesson file loads, with a unique id, a goal and a solution") {
    const std::vector<Lesson> all = lessons();
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(TBCAD_LESSON_DIR))
        files += entry.path().extension() == ".json" ? 1 : 0;
    CHECK(all.size() == files);
    CHECK(all.size() >= 9);
    std::set<std::string> ids;
    for (const Lesson& l : all) {
        CAPTURE(l.id);
        CHECK(ids.insert(l.id).second);
        CHECK(l.world > 0);
        CHECK_FALSE(l.title.en.empty());
        CHECK_FALSE(l.title.th.empty());
        CHECK_FALSE(l.goal.en.empty());
        CHECK_FALSE(l.solution.empty());
        for (const Step& s : l.steps) {
            CHECK_FALSE(s.text.en.empty());
            CHECK_FALSE(s.text.th.empty());
        }
    }
}

TEST_CASE("each world's boss comes after its lessons") {
    const std::vector<Lesson> all = lessons();
    for (std::size_t i = 1; i < all.size(); ++i) {
        CAPTURE(all[i].id);
        if (all[i].world == all[i - 1].world)
            CHECK_FALSE(all[i - 1].boss);  // nothing follows a boss in its world
    }
    CHECK(std::count_if(all.begin(), all.end(), [](const Lesson& l) { return l.boss; }) >= 6);
}

TEST_CASE("every lesson's solution passes within par and its wrong attempts do not") {
    for (const Lesson& l : lessons()) {
        CAPTURE(l.id);
        {
            Practice p("solve");
            p.start(l);
            CHECK_FALSE(p.run->complete());
            std::size_t step = p.run->currentStep();
            for (const std::string& line : l.solution) {
                p.type(line);
                CHECK(p.run->currentStep() >= step);  // steps never come undone
                step = p.run->currentStep();
            }
            CHECK(p.run->complete());
            CHECK(p.run->commandsUsed() <= l.par);
            CHECK(p.run->score(1).stars == 3);
        }
        for (const auto& attempt : l.wrong) {
            Practice p("wrong");
            p.start(l);
            for (const std::string& line : attempt)
                p.type(line);
            CHECK_FALSE(p.run->complete());
        }
    }
}

TEST_CASE("the steps of the first-line lesson tick off one by one") {
    const std::vector<Lesson> all = lessons();
    const auto it =
        std::find_if(all.begin(), all.end(), [](const Lesson& l) { return l.id == "2.1"; });
    REQUIRE(it != all.end());
    Practice p("steps");
    p.start(*it);
    CHECK(p.run->currentStep() == 0);
    p.type("L");
    CHECK(p.run->currentStep() == 1);
    p.type("0,0");
    CHECK(p.run->currentStep() == 2);
    p.type("@1000,0");
    CHECK(p.run->currentStep() == 3);  // the line exists: done without the closing Enter
    CHECK(p.run->complete());
    CHECK(p.run->events().front().source == Source::Alias);
}

TEST_CASE("the corners lesson starts from a sharp corner whatever radius Fillet used before") {
    const std::vector<Lesson> all = lessons();
    const auto it =
        std::find_if(all.begin(), all.end(), [](const Lesson& l) { return l.id == "3.5"; });
    REQUIRE(it != all.end());
    Practice p("fillet");
    p.core->submitScript("FILLET;R;500");  // the radius is remembered between drawings
    p.core->pressEscape();
    p.start(*it);
    for (const std::string& line : it->solution)
        p.type(line);
    CHECK(p.run->complete());
}
