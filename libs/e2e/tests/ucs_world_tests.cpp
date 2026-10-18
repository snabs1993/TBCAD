// The same command script run in the world system and in a rotated UCS: the typed numbers are
// UCS coordinates, so the second drawing must be the first one turned and moved by the UCS.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "tbcad/cmd/register_all.h"
#include "tbcad/dim/dim.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;

namespace {

struct Rig {
    std::filesystem::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;
    drafting::Ucs ucs{};  // world when untouched

    explicit Rig(const char* tag, drafting::Ucs u = {}) : ucs(u) {
        dir = std::filesystem::temp_directory_path() / (std::string("tbcad_ucs_") + tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->applyUiKeybindings(uiRegistry);
        core->setViewportSize(1400, 900);
        core->setSetting("drafting.dynInput", "0");
        core->setSetting("drafting.osnap", "0");
        core->setSetting("drafting.ortho", "0");
        core->setSetting("drafting.polar", "0");
        core->setSetting("drafting.snap", "0");
        core->setSetting("drafting.dynucs", "0");
        if (!u.isWorld())
            core->setSetting(drafting::kUcsSettingKey, drafting::unnamedUcsSetting(u));
        render::Camera2D cam;
        cam.pixelsPerMm = 3;
        cam.viewportW = 1400;
        cam.viewportH = 900;
        cam.center = ucs.toWorld({40, 25});
        core->setCamera(cam);
    }
    ~Rig() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Types the items one by one (each is a line of the command line).
    void run(const std::string& command, std::vector<std::string> items = {}) {
        core->submitText(command);
        for (const std::string& i : items)
            core->submitText(i);
        // A command left waiting is ended, so the next scenario step starts clean.
        for (int k = 0; k < 3 && core->runner().active(); ++k)
            core->submitText("");
        if (core->runner().active())
            core->pressEscape();
    }
    // A mouse click at a UCS-local position (picks objects, or gives a point).
    void click(Vec2 local) {
        core->click(ui::PointerState{core->camera().worldToScreen(ucs.toWorld(local))});
    }
    std::vector<EntityId> ids() const {
        std::vector<EntityId> v;
        core->document().forEachEntity([&](const Entity& e) { v.push_back(e.common.id); });
        std::sort(v.begin(), v.end());
        return v;
    }
    void select(std::initializer_list<std::size_t> indices) {
        const auto all = ids();
        std::vector<EntityId> sel;
        for (std::size_t i : indices)
            if (i < all.size())
                sel.push_back(all[i]);
        core->setSelection(sel);
    }
};

double wrap(double a) {
    a = std::fmod(a, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

struct Diff {
    std::string text;
    bool ok = true;
    void fail(const std::string& s) {
        if (ok)
            text = s;
        ok = false;
    }
};

// Compares the drawing made in the UCS (b) with the one made in the world system (a).
Diff compare(const Rig& a, const Rig& b) {
    Diff d;
    const drafting::Ucs& u = b.ucs;
    const auto ia = a.ids(), ib = b.ids();
    if (ia.size() != ib.size()) {
        d.fail("object count " + std::to_string(ia.size()) + " vs " + std::to_string(ib.size()));
        return d;
    }
    auto close = [](Vec2 p, Vec2 q) { return distance(p, q) < 1e-6; };
    for (std::size_t i = 0; i < ia.size() && d.ok; ++i) {
        const Entity& ea = *a.core->document().find(ia[i]);
        const Entity& eb = *b.core->document().find(ib[i]);
        const std::string at = "#" + std::to_string(i) + " ";
        if (kind(ea) != kind(eb)) {
            d.fail(at + "kind differs");
            break;
        }
        if (const auto* x = entityAs<LineData>(ea)) {
            const auto* y = entityAs<LineData>(eb);
            if (!close(u.toWorld(x->a), y->a) || !close(u.toWorld(x->b), y->b))
                d.fail(at + "line " + std::to_string(x->a.x) + "," + std::to_string(x->a.y) +
                       " -> " + std::to_string(y->a.x) + "," + std::to_string(y->a.y) + " wanted " +
                       std::to_string(u.toWorld(x->a).x) + "," + std::to_string(u.toWorld(x->a).y));
        } else if (const auto* x = entityAs<CircleData>(ea)) {
            const auto* y = entityAs<CircleData>(eb);
            if (!close(u.toWorld(x->center), y->center) || std::abs(x->radius - y->radius) > 1e-6)
                d.fail(at + "circle");
        } else if (const auto* x = entityAs<ArcData>(ea)) {
            const auto* y = entityAs<ArcData>(eb);
            if (!close(u.toWorld(x->center), y->center) || std::abs(x->radius - y->radius) > 1e-6 ||
                std::abs(wrap(x->startAngle + u.angle - y->startAngle + kPi) - kPi) > 1e-6 ||
                std::abs(wrap(x->endAngle + u.angle - y->endAngle + kPi) - kPi) > 1e-6)
                d.fail(at + "arc");
        } else if (const auto* x = entityAs<PolylineData>(ea)) {
            const auto* y = entityAs<PolylineData>(eb);
            if (x->v.size() != y->v.size() || x->closed != y->closed) {
                d.fail(at + "polyline shape");
                break;
            }
            for (std::size_t k = 0; k < x->v.size(); ++k)
                if (!close(u.toWorld(x->v[k].p), y->v[k].p) ||
                    std::abs(x->v[k].bulge - y->v[k].bulge) > 1e-6)
                    d.fail(at + "polyline vertex " + std::to_string(k));
        } else if (const auto* x = entityAs<EllipseData>(ea)) {
            const auto* y = entityAs<EllipseData>(eb);
            if (!close(u.toWorld(x->center), y->center) ||
                !close(u.dirToWorld(x->majorAxis), y->majorAxis) ||
                std::abs(x->ratio - y->ratio) > 1e-6)
                d.fail(at + "ellipse");
        } else if (const auto* x = entityAs<TextData>(ea)) {
            const auto* y = entityAs<TextData>(eb);
            if (!close(u.toWorld(x->position), y->position) || x->utf8 != y->utf8 ||
                std::abs(x->height - y->height) > 1e-9 ||
                std::abs(wrap(x->rotation + u.angle - y->rotation + kPi) - kPi) > 1e-6)
                d.fail(at + "text (rotation " + std::to_string(x->rotation) + " -> " +
                       std::to_string(y->rotation) + ")");
        } else if (const auto* x = entityAs<MTextData>(ea)) {
            const auto* y = entityAs<MTextData>(eb);
            if (!close(u.toWorld(x->position), y->position) || x->utf8 != y->utf8 ||
                std::abs(wrap(x->rotation + u.angle - y->rotation + kPi) - kPi) > 1e-6)
                d.fail(at + "mtext");
        } else if (const auto* x = entityAs<PointData>(ea)) {
            if (!close(u.toWorld(x->p), entityAs<PointData>(eb)->p))
                d.fail(at + "point");
        } else if (const auto* x = entityAs<RayData>(ea)) {
            const auto* y = entityAs<RayData>(eb);
            if (!close(u.toWorld(x->origin), y->origin) || !close(u.dirToWorld(x->dir), y->dir))
                d.fail(at + "ray");
        } else if (const auto* x = entityAs<DimensionData>(ea)) {
            const auto* y = entityAs<DimensionData>(eb);
            if (x->kind != y->kind) {
                d.fail(at + "dimension kind");
                break;
            }
            const DimStyle& sa = dim::styleOf(a.core->document(), x->style);
            const DimStyle& sb = dim::styleOf(b.core->document(), y->style);
            if (dim::dimensionText(sa, *x) != dim::dimensionText(sb, *y))
                d.fail(at + "dimension text '" + dim::dimensionText(sa, *x) + "' vs '" +
                       dim::dimensionText(sb, *y) + "'");
            else if (x->kind == DimKind::Linear &&
                     std::abs(wrap(x->rotation + u.angle - y->rotation + kPi) - kPi) > 1e-6)
                d.fail(at + "linear dimension direction");
        } else if (const auto* x = entityAs<HatchData>(ea)) {
            const auto* y = entityAs<HatchData>(eb);
            if (x->loops.size() != y->loops.size() ||
                (!x->solid &&
                 std::abs(wrap(x->patternAngle + u.angle - y->patternAngle + kPi) - kPi) > 1e-6))
                d.fail(at + "hatch");
        }
    }
    return d;
}

struct Scenario {
    const char* name;
    std::function<void(Rig&)> script;
};

// Two lines and a circle to edit, drawn the same way in both systems.
void base(Rig& r) {
    r.run("LINE", {"0,0", "80,0", "80,50", ""});
    r.run("CIRCLE", {"40,25", "12"});
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> v;
    v.push_back({"LINE and relative polar points",
                 [](Rig& r) { r.run("LINE", {"5,5", "@30,0", "@20<60", "@0,-15", "C"}); }});
    v.push_back({"PLINE closed", [](Rig& r) { r.run("PLINE", {"0,0", "40,0", "40,30", "C"}); }});
    v.push_back({"RECTANGLE", [](Rig& r) { r.run("RECTANGLE", {"10,10", "70,45"}); }});
    v.push_back({"CIRCLE", [](Rig& r) { r.run("CIRCLE", {"20,15", "12.5"}); }});
    v.push_back({"ARC three points", [](Rig& r) { r.run("ARC", {"0,0", "20,15", "40,0"}); }});
    v.push_back({"POLYGON", [](Rig& r) { r.run("POLYGON", {"6", "30,20", "I", "15"}); }});
    v.push_back({"ELLIPSE", [](Rig& r) { r.run("ELLIPSE", {"0,0", "40,0", "12"}); }});
    v.push_back({"POINT", [](Rig& r) { r.run("POINT", {"12,34"}); }});
    v.push_back(
        {"TEXT with rotation 0", [](Rig& r) { r.run("TEXT", {"5,5", "4", "0", "Hello"}); }});
    v.push_back(
        {"TEXT with rotation 25", [](Rig& r) { r.run("TEXT", {"5,5", "4", "25", "Hello"}); }});
    v.push_back({"XLINE through two points", [](Rig& r) { r.run("XLINE", {"0,0", "10,5", ""}); }});
    v.push_back(
        {"DIMLINEAR horizontal", [](Rig& r) { r.run("DIMLINEAR", {"0,0", "80,30", "40,-20"}); }});
    v.push_back(
        {"DIMLINEAR vertical", [](Rig& r) { r.run("DIMLINEAR", {"0,0", "30,80", "-20,40"}); }});
    v.push_back({"DIMALIGNED", [](Rig& r) { r.run("DIMALIGNED", {"0,0", "80,30", "40,-20"}); }});
    v.push_back({"DIMORDINATE", [](Rig& r) { r.run("DIMORDINATE", {"12,34", "12,60"}); }});
    v.push_back({"DIMANGULAR three points",
                 [](Rig& r) { r.run("DIMANGULAR", {"", "0,0", "50,0", "0,50", "30,30"}); }});
    v.push_back({"MOVE", [](Rig& r) {
                     base(r);
                     r.select({0, 1});
                     r.run("MOVE", {"0,0", "15,-10"});
                 }});
    v.push_back({"COPY", [](Rig& r) {
                     base(r);
                     r.select({0, 1});
                     r.run("COPY", {"0,0", "15,-10", ""});
                 }});
    v.push_back({"ROTATE", [](Rig& r) {
                     base(r);
                     r.select({0, 1});
                     r.run("ROTATE", {"10,10", "40"});
                 }});
    v.push_back({"SCALE", [](Rig& r) {
                     base(r);
                     r.select({0, 1});
                     r.run("SCALE", {"10,10", "1.5"});
                 }});
    v.push_back({"MIRROR", [](Rig& r) {
                     base(r);
                     r.select({0, 1});
                     r.run("MIRROR", {"0,0", "0,10", "N"});
                 }});
    v.push_back({"OFFSET", [](Rig& r) {
                     base(r);
                     r.run("OFFSET", {"7"});
                     r.click({40, 0});
                     r.click({40, 10});
                     r.core->submitText("");
                 }});
    v.push_back({"FILLET", [](Rig& r) {
                     r.run("LINE", {"0,0", "80,0", ""});
                     r.run("LINE", {"0,0", "0,60", ""});
                     r.run("FILLET", {"R", "10"});
                     r.run("FILLET");
                     r.click({60, 0});
                     r.click({0, 40});
                 }});
    v.push_back({"ARRAY rectangular", [](Rig& r) {
                     r.run("RECTANGLE", {"0,0", "10,6"});
                     r.select({0});
                     r.run("ARRAYRECT", {"2", "3", "15", "20"});
                 }});
    return v;
}

}  // namespace

TEST_CASE("commands in a rotated UCS give the world result turned by the UCS") {
    const drafting::Ucs tilt{"", {120, -45}, 30 * kPi / 180};
    for (const Scenario& s : scenarios()) {
        const std::string name = s.name;
        CAPTURE(name);
        // One after the other: the drafting module keeps a single set of filters, and they look
        // at the drawing of the AppCore that was set up last.
        Rig world("w");
        s.script(world);
        Rig turned("t", tilt);
        s.script(turned);
        const Diff d = compare(world, turned);
        INFO(d.text);
        CHECK(d.ok);
    }
}

TEST_CASE("UCS command options: 3point, Previous, View, Save and restore, UCSICON, PLAN") {
    Rig r("cmds");
    auto cur = [&] { return drafting::currentUcs(&r.core->document(), r.core.get()); };
    r.run("UCS", {"3point", "10,20", "20,30", ""});
    CHECK(cur().origin.x == doctest::Approx(10));
    CHECK(cur().origin.y == doctest::Approx(20));
    CHECK(cur().angle == doctest::Approx(kPi / 4));
    r.run("UCS", {"Save", "Diag"});
    r.run("UCS", {"World"});
    CHECK(cur().isWorld());
    r.run("UCS", {"Previous"});
    CHECK(cur().name == "Diag");
    r.run("UCS", {"View"});
    CHECK(cur().angle == doctest::Approx(0));
    CHECK(cur().origin.x == doctest::Approx(10));
    r.run("UCS", {"Restore", "Diag"});
    CHECK(cur().angle == doctest::Approx(kPi / 4));
    r.run("UCSICON", {"OFF"});
    CHECK(r.core->setting("drafting.ucsicon").value_or("") == "0");
    r.run("PLAN", {"World"});
    CHECK(r.core->camera().center.x == doctest::Approx(0));
    CHECK(r.core->camera().center.y == doctest::Approx(0));
}
