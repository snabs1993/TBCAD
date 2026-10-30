#include <doctest/doctest.h>

#include <fstream>

#include "practice.h"
#include "tbcad/training/badges.h"

using namespace tbcad;
using namespace tbcad::training;
using namespace tbcad::training::test;

namespace {

Check parseOne(const std::string& json) {
    auto l = parseLesson(R"({"id": "t", "steps": [{"text": "x", "checks": [)" + json + "]}]}");
    if (!l.ok())
        FAIL(l.error().message);
    return l.value().steps[0].checks[0];
}

bool passesOn(Practice& p, const std::string& json) {
    return passes(parseOne(json), p.facts());
}

}  // namespace

TEST_CASE("how a command was started") {
    CHECK(classifySource("", true, "LINE") == Source::Button);
    CHECK(classifySource("LINE", false, "LINE") == Source::Name);
    CHECK(classifySource("line", false, "LINE") == Source::Name);
    CHECK(classifySource("L", false, "LINE") == Source::Alias);
    CHECK(classifySource("เส้น", false, "LINE") == Source::Thai);
    CHECK(classifySource("", false, "LINE") == Source::Other);
}

TEST_CASE("stars: finish, no hint, within par and time") {
    Lesson l;
    l.par = 2;
    CHECK(starsFor(l, false, false, 0, 1) == 0);
    CHECK(starsFor(l, true, true, 1, 1) == 1);
    CHECK(starsFor(l, true, false, 3, 1) == 2);
    CHECK(starsFor(l, true, false, 2, 1) == 3);
    l.parSeconds = 60;
    CHECK(starsFor(l, true, false, 2, 61) == 2);
    CHECK(starsFor(l, true, false, 2, 59) == 3);
    CHECK_FALSE(countsTowardsPar("ZOOM"));
    CHECK_FALSE(countsTowardsPar("U"));
    CHECK(countsTowardsPar("LINE"));
}

TEST_CASE("progress keeps the best result and survives a round trip through JSON") {
    Progress p;
    CHECK_FALSE(p.record("2.1", Score{0, 5, 1, 0, false}));
    CHECK(p.record("2.1", Score{2, 30, 3, 0, false}));
    CHECK(p.record("2.1", Score{1, 20, 3, 0, true}));  // faster, fewer stars
    CHECK(p.stars("2.1") == 2);
    CHECK(p.lessons["2.1"].bestSeconds == doctest::Approx(20));
    CHECK_FALSE(p.record("2.1", Score{1, 25, 3, 0, true}));
    p.badges.insert("first-line");
    const Progress q = Progress::fromJson(p.toJson());
    CHECK(q.stars("2.1") == 2);
    CHECK(q.lessons.at("2.1").bestSeconds == doctest::Approx(20));
    CHECK(q.badges.count("first-line") == 1);
    CHECK(Progress::fromJson("not json").lessons.empty());
    CHECK(Progress::fromJson(R"({"lessons": {"1.1": {"stars": 9}}})").stars("1.1") == 3);
}

TEST_CASE("lesson files that are wrong say why") {
    CHECK_FALSE(parseLesson("[]").ok());
    CHECK_FALSE(parseLesson(R"({"steps": []})").ok());                          // no id
    CHECK_FALSE(parseLesson(R"({"id": "x", "steps": [{"text": "a"}]})").ok());  // no checks
    auto bad = parseLesson(R"({"id": "x", "steps": [{"checks": [{"type": "teleport"}]}]})");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("teleport") != std::string::npos);
    CHECK_FALSE(
        parseLesson(R"({"id": "x", "steps": [{"checks": [{"type": "path", "path": [[0,0]]}]}]})")
            .ok());
    CHECK_FALSE(
        parseLesson(
            R"({"id": "x", "ghosts": [{"star": 1}], "steps": [{"checks": [{"type": "undo"}]}]})")
            .ok());
}

TEST_CASE("lessons load in world and number order") {
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_training_order";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto write = [&](const char* file, const char* id, int world) {
        std::ofstream(dir / file) << R"({"id": ")" << id << R"(", "world": )" << world
                                  << R"(, "steps": [{"text": "a", "checks": [{"type": "undo"}]}]})";
    };
    write("a.json", "2.10", 2);
    write("b.json", "2.9", 2);
    write("c.json", "1.1", 1);
    std::ofstream(dir / "d.json") << "{";
    std::ofstream(dir / "notes.txt") << "ignored";
    std::vector<std::string> errors;
    const auto all = loadLessons(dir, &errors);
    REQUIRE(all.size() == 3);
    CHECK(all[0].id == "1.1");
    CHECK(all[1].id == "2.9");
    CHECK(all[2].id == "2.10");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("d.json") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a later step that passes ticks off the steps before it, and steps never come undone") {
    auto l = parseLesson(R"({"id": "t", "steps": [
        {"text": "start", "checks": [{"type": "prompt", "command": "LINE"}]},
        {"text": "guidance only"},
        {"text": "draw", "checks": [{"type": "count", "kind": "line", "min": 1}]}]})");
    REQUIRE(l.ok());
    Practice p("steplogic");
    p.start(l.value());
    CHECK(p.run->currentStep() == 0);
    p.type("LINE");
    CHECK(p.run->currentStep() == 1);  // the guidance step waits for the next one
    p.type("0,0");
    p.type("10,0");
    CHECK(p.run->complete());
    p.core->undo();
    p.run->update(p.facts());
    CHECK(p.run->complete());
}

TEST_CASE("geometry checks: lines, paths, circles, polylines and counts") {
    Practice p("geom");
    p.core->submitScript("LINE;0,0;100,0;");
    p.core->submitScript("PLINE;0,50;100,50;100,150;");
    p.core->submitScript("CIRCLE;300,0;25");
    p.core->submitScript("RECTANGLE;500,0;@200,100");

    CHECK(passesOn(p, R"({"type": "line", "from": [100, 0], "to": [0, 0]})"));
    CHECK_FALSE(passesOn(p, R"({"type": "line", "from": [0, 0], "to": [101, 0]})"));
    CHECK(passesOn(p, R"({"type": "line", "from": [0, 0], "length": 100, "angle": 0})"));
    CHECK_FALSE(passesOn(p, R"({"type": "line", "from": [0, 0], "length": 100, "angle": 180})"));
    CHECK(passesOn(p, R"({"type": "line", "length": 100, "angle": 270})"));          // either way
    CHECK(passesOn(p, R"({"type": "line", "from": [100, 50], "to": [100, 150]})"));  // pline piece
    CHECK(passesOn(p, R"({"type": "path", "path": [[0, 50], [50, 50], [100, 50], [100, 150]]})"));
    CHECK_FALSE(passesOn(
        p, R"({"type": "path", "path": [[0, 50], [100, 50], [100, 150]], "closed": true})"));
    CHECK(passesOn(p, R"({"type": "circle", "center": [300, 0], "radius": 25})"));
    CHECK_FALSE(passesOn(p, R"({"type": "circle", "center": [300, 0], "radius": 30})"));
    CHECK(passesOn(
        p,
        R"({"type": "polyline", "closed": true, "vertices": 4, "box": [[500, 0], [700, 100]], "area": 20000})"));
    CHECK_FALSE(passesOn(p, R"({"type": "polyline", "closed": true, "arcs": 1})"));
    CHECK(passesOn(p, R"({"type": "count", "kind": "polyline", "equals": 2})"));
    CHECK(passesOn(p, R"({"type": "count", "equals": 4})"));
    CHECK(passesOn(p, R"({"type": "count", "kind": "text", "equals": 0})"));
    CHECK_FALSE(passesOn(p, R"({"type": "count", "kind": "circle", "min": 2})"));
}

TEST_CASE("annotation checks: arcs, text, leader notes, dimensions and object extents") {
    Practice p("notes");
    p.core->submitScript("ARC;C;0,0;100,0;0,100");
    p.core->submitScript("TEXT;10,10;25;0;ห้องนอน");
    p.core->submitScript("MLEADER;500,0;600,100;Steel");
    p.core->submitScript("DIMLINEAR;0,0;400,0;200,-50");
    p.core->submitScript("LINE;1000,0;1100,0;");
    p.core->submitScript("LINE;1000,0;1000,100;");
    p.core->submitScript("DIMANGULAR;;1000,0;1100,0;1000,100;1080,80");
    p.core->submitScript("RECTANGLE;2000,0;@300,200");
    p.core->submitScript("HATCH;2150,100;");

    CHECK(passesOn(p, R"({"type": "arc", "center": [0, 0], "radius": 100})"));
    CHECK_FALSE(passesOn(p, R"({"type": "arc", "radius": 90})"));

    CHECK(passesOn(
        p, R"({"type": "text", "contains": "ห้อง", "height": 25, "box": [[0, 0], [50, 50]]})"));
    CHECK_FALSE(passesOn(p, R"({"type": "text", "contains": "ห้อง", "height": 30})"));
    CHECK_FALSE(
        passesOn(p, R"({"type": "text", "contains": "ห้อง", "box": [[100, 0], [150, 50]]})"));
    CHECK(passesOn(p, R"({"type": "text", "contains": "Steel", "tip": [[490, -10], [510, 10]]})"));
    CHECK_FALSE(passesOn(p, R"({"type": "text", "tip": [[590, 90], [610, 110]]})"));
    CHECK_FALSE(passesOn(p, R"({"type": "text", "contains": "Steel", "height": 25})"));

    CHECK(passesOn(p, R"({"type": "dimension", "dim": "linear", "value": 400})"));
    CHECK_FALSE(passesOn(p, R"({"type": "dimension", "dim": "aligned", "value": 400})"));
    CHECK(passesOn(p, R"({"type": "dimension", "dim": "angular", "value": 90})"));
    CHECK_FALSE(passesOn(p, R"({"type": "dimension", "value": 45})"));
    CHECK_FALSE(
        parseLesson(
            R"({"id": "x", "steps": [{"checks": [{"type": "dimension", "dim": "curly"}]}]})")
            .ok());

    CHECK(passesOn(p, R"({"type": "object", "kind": "hatch", "box": [[2000, 0], [2300, 200]]})"));
    CHECK_FALSE(
        passesOn(p, R"({"type": "object", "kind": "hatch", "box": [[2000, 0], [2400, 200]]})"));
}

TEST_CASE("layer, colour, selection, view and prompt checks") {
    Practice p("props");
    p.core->submitScript("-LAYER;New;WALL;Color;1;WALL;");
    p.core->submitScript("COLOR;3");
    p.core->submitScript("CIRCLE;0,0;10");
    p.core->submitScript("COLOR;ByLayer");
    p.core->submitScript("CIRCLE;100,0;10");
    CHECK(passesOn(
        p, R"({"type": "layer", "name": "wall", "color": 1, "on": true, "locked": false})"));
    CHECK_FALSE(passesOn(p, R"({"type": "layer", "name": "WALL", "color": 2})"));
    CHECK_FALSE(passesOn(p, R"({"type": "layer", "name": "DOOR"})"));
    CHECK(passesOn(p, R"({"type": "count", "kind": "circle", "color": 3, "equals": 1})"));
    CHECK(passesOn(
        p, R"({"type": "count", "kind": "circle", "color": 7, "equals": 1})"));  // ByLayer on 0

    p.core->selectAll();
    CHECK(passesOn(p, R"({"type": "selection", "kind": "circle", "equals": 2})"));
    CHECK_FALSE(passesOn(p, R"({"type": "selection", "color": 3, "equals": 2})"));

    p.core->zoomToBox(BBox2{{-20, -20}, {20, 20}});
    CHECK(passesOn(p, R"({"type": "view", "box": [[-10, -10], [10, 10]], "minFill": 0.3})"));
    CHECK_FALSE(passesOn(p, R"({"type": "view", "box": [[-10, -10], [110, 10]]})"));

    CHECK_FALSE(passesOn(p, R"({"type": "prompt", "command": "LINE"})"));
    p.core->submitText("LINE");
    CHECK(passesOn(p, R"({"type": "prompt", "command": "line", "contains": "FIRST POINT"})"));
    CHECK_FALSE(passesOn(p, R"({"type": "prompt", "command": "CIRCLE"})"));
}

TEST_CASE("command, undo and redo checks count what the user did") {
    auto l = parseLesson(R"({"id": "t", "par": 1, "steps": [{"text": "x", "checks": [
        {"type": "command", "command": "CIRCLE", "source": "alias"},
        {"type": "command", "command": "LINE", "source": "button"},
        {"type": "undo"}, {"type": "redo"}]}]})");
    REQUIRE(l.ok());
    Practice p("events");
    p.start(l.value());
    p.type("C;0,0;10");
    p.core->startCommand("LINE");
    p.core->pressEscape();
    p.run->update(p.facts());
    CHECK_FALSE(p.run->complete());
    p.core->undo();
    p.core->redo();
    p.run->update(p.facts());
    CHECK(p.run->complete());
    CHECK(p.run->commandsUsed() == 2);
    CHECK(p.run->undos() == 1);
    CHECK(p.run->score(1).stars == 2);  // over par
}

TEST_CASE("block and insert checks, and an array counts as the objects it shows") {
    Practice p("blocks");
    p.core->submitScript("RECTANGLE;0,0;@100,100");
    p.core->submitScript("-BLOCK;BOX;ALL;;0,0;Delete");
    p.core->submitScript("-INSERT;BOX;R;90;500,0");
    p.core->submitScript("CIRCLE;1000,0;10");
    p.core->submitScript("ARRAYRECT;L;;COU;3;2;S;50;80;");
    CHECK(passesOn(p, R"({"type": "block", "name": "box", "objects": 1})"));
    CHECK_FALSE(passesOn(p, R"({"type": "block", "name": "BOX", "objects": 2})"));
    CHECK_FALSE(passesOn(p, R"({"type": "block", "name": "CHAIR"})"));
    CHECK(passesOn(p, R"({"type": "insert", "name": "BOX", "center": [500, 0], "angle": 90})"));
    CHECK_FALSE(passesOn(p, R"({"type": "insert", "name": "BOX", "angle": 0})"));
    CHECK(passesOn(p, R"({"type": "insert", "equals": 1})"));  // the array is not an insert here
    CHECK(passesOn(p, R"({"type": "count", "kind": "circle", "equals": 6})"));
    CHECK(passesOn(p, R"({"type": "circle", "center": [1100, 80], "radius": 10})"));
}

TEST_CASE("badges: earned once, some counted across lessons") {
    Lesson first;
    first.id = "2.1";
    first.world = 2;
    first.par = 1;
    Lesson other;
    other.id = "2.2";
    other.world = 2;
    const std::vector<Lesson> all{first, other};
    Progress pr;
    Facts f;
    auto finish = [&](const Lesson& l, std::vector<Event> events) {
        LessonRun run(l);
        for (const Event& e : events)
            run.record(e);
        pr.record(l.id, run.score(1));
        return awardBadges(pr, all, l, run, f);
    };
    const Event typed{Event::Kind::Started, "LINE", Source::Alias};
    const Event button{Event::Kind::Started, "LINE", Source::Button};
    const Event thai{Event::Kind::Started, "LINE", Source::Thai};
    const Event undo{Event::Kind::Undo, "U", Source::Name};

    auto earned = finish(first, {typed});
    CHECK(std::find(earned.begin(), earned.end(), "first-line") != earned.end());
    CHECK(finish(first, {typed}).empty());  // once only
    earned = finish(other, {button, thai});
    CHECK(std::find(earned.begin(), earned.end(), "three-ways") != earned.end());
    CHECK(std::find(earned.begin(), earned.end(), "no-undo") != earned.end());  // 3 in a row
    CHECK(pr.typedLessons == 2);  // the button start broke the third
    finish(other, {typed, undo});
    CHECK(pr.noUndoStreak == 0);
    finish(other, {thai});
    earned = finish(other, {typed});
    CHECK(std::find(earned.begin(), earned.end(), "keyboard-pro") != earned.end());

    const Progress back = Progress::fromJson(pr.toJson());
    CHECK(back.typedLessons == pr.typedLessons);
    CHECK(back.ways.at("LINE") == 7);
    CHECK(back.badges == pr.badges);
    CHECK(findBadge("graduate") != nullptr);
    CHECK(findBadge("nope") == nullptr);
}
