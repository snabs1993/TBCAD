#include "tbcad/training/lesson.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "tbcad/platform/json.h"

namespace tbcad::training {

namespace {

using platform::Json;

Error parseError(const std::string& what) {
    return Error{ErrorCode::Parse, what};
}

Text text(const Json* j) {
    Text t;
    if (!j)
        return t;
    if (j->isString()) {
        t.en = j->asString();
        return t;
    }
    if (const Json* en = j->find("en"))
        t.en = en->asString();
    if (const Json* th = j->find("th"))
        t.th = th->asString();
    return t;
}

std::optional<Vec2> point(const Json* j) {
    if (!j || !j->isArray() || j->asArray().size() < 2)
        return std::nullopt;
    const Json::Array& a = j->asArray();
    if (!a[0].isNumber() || !a[1].isNumber())
        return std::nullopt;
    return Vec2{a[0].asNumber(), a[1].asNumber()};
}

std::optional<double> number(const Json& o, const char* key) {
    const Json* j = o.find(key);
    if (!j || !j->isNumber())
        return std::nullopt;
    return j->asNumber();
}

std::optional<int> integer(const Json& o, const char* key) {
    if (auto n = number(o, key))
        return static_cast<int>(*n);
    return std::nullopt;
}

std::optional<bool> boolean(const Json& o, const char* key) {
    const Json* j = o.find(key);
    if (!j || !j->isBool())
        return std::nullopt;
    return j->asBool();
}

std::string string(const Json& o, const char* key) {
    const Json* j = o.find(key);
    return j ? j->asString() : std::string();
}

std::vector<std::string> strings(const Json* j) {
    std::vector<std::string> out;
    if (!j)
        return out;
    if (j->isString())
        out.push_back(j->asString());
    for (const Json& s : j->asArray())
        out.push_back(s.asString());
    return out;
}

Result<std::vector<Vec2>> points(const Json* j, const char* what) {
    std::vector<Vec2> out;
    if (!j)
        return out;
    for (const Json& p : j->asArray()) {
        auto v = point(&p);
        if (!v)
            return parseError(std::string(what) + ": every point must be [x, y]");
        out.push_back(*v);
    }
    return out;
}

Result<Check::Type> checkType(const std::string& s) {
    static const std::pair<const char*, Check::Type> kTypes[] = {
        {"line", Check::Type::Line},         {"circle", Check::Type::Circle},
        {"polyline", Check::Type::Polyline}, {"path", Check::Type::Path},
        {"count", Check::Type::Count},       {"layer", Check::Type::Layer},
        {"command", Check::Type::Command},   {"undo", Check::Type::Undo},
        {"redo", Check::Type::Redo},         {"selection", Check::Type::Selection},
        {"view", Check::Type::View},         {"prompt", Check::Type::Prompt},
    };
    for (const auto& [name, type] : kTypes)
        if (s == name)
            return type;
    return parseError("unknown check type \"" + s + "\"");
}

Result<Check> parseCheck(const Json& o) {
    auto type = checkType(string(o, "type"));
    if (!type)
        return type.error();
    Check c;
    c.type = type.value();
    c.from = point(o.find("from"));
    c.to = point(o.find("to"));
    c.center = point(o.find("center"));
    c.length = number(o, "length");
    c.angle = number(o, "angle");
    c.radius = number(o, "radius");
    c.area = number(o, "area");
    if (const Json* b = o.find("box")) {
        auto corners = points(b, "box");
        if (!corners || corners.value().size() != 2)
            return parseError("box must be [[x0, y0], [x1, y1]]");
        BBox2 box;
        box.expand(corners.value()[0]);
        box.expand(corners.value()[1]);
        c.box = box;
    }
    auto path = points(o.find("path"), "path");
    if (!path)
        return path.error();
    c.path = std::move(path).value();
    c.closed = boolean(o, "closed");
    c.vertices = integer(o, "vertices");
    c.arcs = integer(o, "arcs");
    c.tol = number(o, "tol").value_or(c.tol);
    c.angleTol = number(o, "angleTol").value_or(c.angleTol);
    c.areaTolPct = number(o, "areaTolPct").value_or(c.areaTolPct);
    c.kind = string(o, "kind");
    c.layer = string(o, "layer");
    c.color = integer(o, "color");
    c.equals = integer(o, "equals");
    c.min = integer(o, "min");
    c.max = integer(o, "max");
    c.name = string(o, "name");
    c.on = boolean(o, "on");
    c.frozen = boolean(o, "frozen");
    c.locked = boolean(o, "locked");
    c.command = string(o, "command");
    c.source = string(o, "source");
    c.contains = string(o, "contains");
    c.minFill = number(o, "minFill").value_or(0);

    switch (c.type) {
        case Check::Type::Path:
            if (c.path.size() < 2)
                return parseError("a path check needs at least 2 points");
            break;
        case Check::Type::Layer:
            if (c.name.empty())
                return parseError("a layer check needs a name");
            break;
        case Check::Type::Command:
            if (c.command.empty())
                return parseError("a command check needs a command");
            break;
        case Check::Type::View:
            if (!c.box)
                return parseError("a view check needs a box");
            break;
        default:
            break;
    }
    return c;
}

Result<Ghost> parseGhost(const Json& o) {
    Ghost g;
    if (const Json* l = o.find("line")) {
        auto p = points(l, "line ghost");
        if (!p || p.value().size() != 2)
            return parseError("a line ghost is [[x0, y0], [x1, y1]]");
        g.kind = Ghost::Kind::Line;
        g.points = std::move(p).value();
    } else if (const Json* c = o.find("circle")) {
        g.kind = Ghost::Kind::Circle;
        auto ctr = point(c);
        if (!ctr)
            return parseError("a circle ghost is [x, y] with a radius");
        g.points = {*ctr};
        g.radius = number(o, "radius").value_or(0);
    } else if (const Json* d = o.find("dot")) {
        g.kind = Ghost::Kind::Dot;
        auto p = point(d);
        if (!p)
            return parseError("a dot ghost is [x, y]");
        g.points = {*p};
    } else if (const Json* pl = o.find("polyline")) {
        g.kind = Ghost::Kind::Polyline;
        for (const Json& v : pl->asArray()) {
            auto p = point(&v);
            if (!p)
                return parseError("a polyline ghost vertex is [x, y] or [x, y, bulge]");
            g.points.push_back(*p);
            const Json::Array& a = v.asArray();
            g.bulges.push_back(a.size() > 2 ? a[2].asNumber() : 0.0);
        }
        if (g.points.size() < 2)
            return parseError("a polyline ghost needs 2 vertices");
        g.closed = boolean(o, "closed").value_or(false);
    } else {
        return parseError("a ghost is a line, circle, dot or polyline");
    }
    return g;
}

}  // namespace

Result<Lesson> parseLesson(std::string_view json) {
    auto parsed = platform::parseJson(json);
    if (!parsed)
        return parsed.error();
    const Json& root = parsed.value();
    if (!root.isObject())
        return parseError("a lesson is a JSON object");
    Lesson l;
    l.id = string(root, "id");
    if (l.id.empty())
        return parseError("the lesson has no id");
    l.world = integer(root, "world").value_or(0);
    l.title = text(root.find("title"));
    l.goal = text(root.find("goal"));
    l.tip = text(root.find("tip"));
    l.par = integer(root, "par").value_or(0);
    l.parSeconds = number(root, "parSeconds").value_or(0);
    l.setup = strings(root.find("setup"));
    if (const Json* gs = root.find("ghosts"))
        for (const Json& g : gs->asArray()) {
            auto ghost = parseGhost(g);
            if (!ghost)
                return parseError("lesson " + l.id + ": " + ghost.error().message);
            l.ghosts.push_back(std::move(ghost).value());
        }
    if (const Json* ss = root.find("steps"))
        for (const Json& s : ss->asArray()) {
            Step step;
            step.text = text(s.find("text"));
            step.hint = text(s.find("hint"));
            if (const Json* cs = s.find("checks"))
                for (const Json& c : cs->asArray()) {
                    auto check = parseCheck(c);
                    if (!check)
                        return parseError("lesson " + l.id + ": " + check.error().message);
                    step.checks.push_back(std::move(check).value());
                }
            l.steps.push_back(std::move(step));
        }
    if (l.steps.empty() || l.steps.back().checks.empty())
        return parseError("lesson " + l.id + ": the last step must have checks");
    l.solution = strings(root.find("solution"));
    if (const Json* ws = root.find("wrong"))
        for (const Json& w : ws->asArray())
            l.wrong.push_back(strings(&w));
    return l;
}

Result<Lesson> loadLesson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Error{ErrorCode::Io, "cannot read " + file.string()};
    std::stringstream ss;
    ss << in.rdbuf();
    auto l = parseLesson(ss.str());
    if (!l)
        return Error{l.error().code, file.filename().string() + ": " + l.error().message};
    return l;
}

std::vector<Lesson> loadLessons(const std::filesystem::path& folder,
                                std::vector<std::string>* errors) {
    std::vector<Lesson> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        auto l = loadLesson(entry.path());
        if (l)
            out.push_back(std::move(l).value());
        else if (errors)
            errors->push_back(l.error().message);
    }
    if (ec && errors)
        errors->push_back("cannot list " + folder.string() + ": " + ec.message());
    // "2.10" sorts after "2.9": compare the dot-separated parts as numbers.
    auto parts = [](const std::string& id) {
        std::vector<long> v;
        std::stringstream ss(id);
        for (std::string p; std::getline(ss, p, '.');)
            v.push_back(std::strtol(p.c_str(), nullptr, 10));
        return v;
    };
    std::sort(out.begin(), out.end(), [&](const Lesson& a, const Lesson& b) {
        if (a.world != b.world)
            return a.world < b.world;
        return parts(a.id) < parts(b.id);
    });
    return out;
}

}  // namespace tbcad::training
