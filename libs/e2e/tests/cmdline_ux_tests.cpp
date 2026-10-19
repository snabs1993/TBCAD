// The command line the way a drafter uses it: AutoCAD's aliases, repeating the last command,
// transparent commands inside another command, and typed @relative, <polar and #absolute points.
// Runs the real command set through AppCore, with the drafting module's UI hooks.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

struct Cli {
    fs::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;

    Cli() {
        dir = fs::temp_directory_path() / ("tbcad_cmdline_ux_" + std::to_string(std::rand()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->applyUiKeybindings(uiRegistry);
        core->setViewportSize(1000, 800);
    }
    ~Cli() {
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void say(std::initializer_list<const char*> lines) {
        for (const char* l : lines)
            core->submitText(l);
    }
    std::vector<LineData> lines() {
        std::vector<LineData> out;
        core->document().forEachEntity([&](const Entity& e) {
            if (const auto* l = entityAs<LineData>(e))
                out.push_back(*l);
        });
        return out;
    }
    bool running() { return core->runner().active(); }
};

bool near(Vec2 a, Vec2 b) {
    return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6;
}

}  // namespace

TEST_CASE("AutoCAD's everyday aliases are there") {
    Cli c;
    // alias -> command, from the default acad.pgp.
    const std::pair<const char*, const char*> expected[] = {
        {"L", "LINE"},          {"C", "CIRCLE"},        {"A", "ARC"},
        {"PL", "PLINE"},        {"REC", "RECTANG"},     {"POL", "POLYGON"},
        {"EL", "ELLIPSE"},      {"SPL", "SPLINE"},      {"XL", "XLINE"},
        {"H", "HATCH"},         {"T", "MTEXT"},         {"MT", "MTEXT"},
        {"DT", "TEXT"},         {"B", "BLOCK"},         {"I", "INSERT"},
        {"W", "WBLOCK"},        {"M", "MOVE"},          {"CO", "COPY"},
        {"E", "ERASE"},         {"RO", "ROTATE"},       {"SC", "SCALE"},
        {"MI", "MIRROR"},       {"O", "OFFSET"},        {"TR", "TRIM"},
        {"EX", "EXTEND"},       {"F", "FILLET"},        {"CHA", "CHAMFER"},
        {"S", "STRETCH"},       {"AR", "ARRAY"},        {"BR", "BREAK"},
        {"J", "JOIN"},          {"X", "EXPLODE"},       {"PE", "PEDIT"},
        {"LEN", "LENGTHEN"},    {"AL", "ALIGN"},        {"U", "UNDO"},
        {"Z", "ZOOM"},          {"P", "PAN"},           {"RE", "REGEN"},
        {"LA", "LAYER"},        {"LT", "LINETYPE"},     {"DI", "DIST"},
        {"ID", "ID"},           {"AA", "AREA"},         {"LI", "LIST"},
        {"MA", "MATCHPROP"},    {"PU", "PURGE"},        {"OP", "OPTIONS"},
        {"D", "DIMSTYLE"},      {"DLI", "DIMLINEAR"},   {"DAL", "DIMALIGNED"},
        {"DRA", "DIMRADIUS"},   {"DDI", "DIMDIAMETER"}, {"DAN", "DIMANGULAR"},
        {"DCO", "DIMCONTINUE"}, {"ST", "STYLE"},        {"UN", "UNITS"},
        {"MS", "MSPACE"},       {"PS", "PSPACE"},       {"CP", "COPY"},
        {"PR", "PROPERTIES"},   {"CH", "PROPERTIES"},   {"DIV", "DIVIDE"},
        {"PO", "POINT"},        {"DO", "DONUT"},        {"REG", "REGION"},
        {"BO", "BOUNDARY"},     {"MLD", "MLEADER"},     {"TB", "TABLE"},
        {"SU", "SUBTRACT"},     {"UNI", "UNION"},       {"IN", "INTERSECT"},
        {"R", "REDRAW"},        {"RA", "REDRAWALL"},    {"DS", "DSETTINGS"},
        {"OS", "OSNAP"},        {"PL", "PLINE"},        {"EXIT", "QUIT"},
        {"FI", "FILTER"},       {"LO", "LAYOUT"},       {"SP", "SPELL"},
        {"NCOPY", "NCOPY"},     {"ATT", "ATTDEF"},      {"ATE", "ATTEDIT"},
        {"XR", "XREF"},         {"XA", "XATTACH"},      {"XB", "XBIND"},
        {"GR", "DDGRIPS"},      {"DV", "DVIEW"},        {"-INSERT", "INSERT"},
        {"-BLOCK", "BLOCK"},    {"-HATCH", "HATCH"},    {"-BOUNDARY", "BOUNDARY"},
        {"-LAYER", "-LAYER"}};
    std::string missing;
    for (const auto& [alias, command] : expected) {
        const cmd::Command* byAlias = c.registry.find(alias);
        const cmd::Command* target = c.registry.find(command);
        if (!target)
            continue;  // the command itself is not in this program (yet): not a command-line matter
        if (!byAlias || byAlias != target)
            missing += std::string(" ") + alias + "=" + command;
    }
    INFO("missing or different:" << missing);
    CHECK(missing.empty());
}

TEST_CASE("Enter and Space on an empty line repeat the last command") {
    Cli c;
    c.say({"LINE", "0,0", "10,0", ""});
    CHECK_FALSE(c.running());
    CHECK(c.lines().size() == 1);
    c.say({""});  // repeats LINE
    CHECK(c.running());
    c.say({"20,0", "30,0", ""});
    CHECK(c.lines().size() == 2);
    c.say({" "});  // a space alone is Enter too
    CHECK(c.running());
    c.core->pressEscape();
    CHECK_FALSE(c.running());
}

TEST_CASE("typed points: absolute, @relative, <polar and #absolute") {
    Cli c;
    c.say({"LINE", "5,5", "@10,0", "@10<90", "#40,40", "20<180", "@", ""});
    const auto l = c.lines();
    REQUIRE(l.size() == 5);
    CHECK(near(l[0].a, {5, 5}));
    CHECK(near(l[0].b, {15, 5}));
    CHECK(near(l[1].b, {15, 15}));
    CHECK(near(l[2].b, {40, 40}));
    CHECK(near(l[3].b, {-20, 0}));  // 20<180 is absolute polar from the origin
    // "@" alone is the last point: a zero-length segment is not drawn, so it ended the chain.
}

TEST_CASE("transparent commands run inside another command and hand control back") {
    Cli c;
    c.say({"LINE", "0,0"});
    REQUIRE(c.running());
    c.say({"'ZOOM", "E"});
    CHECK(c.running());  // still LINE
    c.say({"'PAN", "0,0", "5,5"});
    CHECK(c.running());
    c.say({"10,0", ""});
    CHECK_FALSE(c.running());
    REQUIRE(c.lines().size() == 1);
    CHECK(near(c.lines()[0].b, {10, 0}));
}

TEST_CASE("a command that is not transparent is refused in the middle of another") {
    Cli c;
    c.say({"LINE", "0,0"});
    c.say({"'CIRCLE"});
    CHECK(c.running());
    c.core->pressEscape();
    c.core->pressEscape();
    CHECK_FALSE(c.running());
    CHECK(c.lines().empty());
}

TEST_CASE("keyword options take their capital letters and unique prefixes, any case") {
    Cli c;
    c.say({"ZOOM", "e"});
    CHECK_FALSE(c.running());
    c.say({"zoom", "ext"});
    CHECK_FALSE(c.running());
    c.say({"line", "0,0", "10,0", "10,10", "c"});  // Close ends the chain
    CHECK_FALSE(c.running());
}

TEST_CASE("@ without a command in progress is relative to the last point") {
    Cli c;
    c.say({"LINE", "5,5", "10,5", ""});
    c.say({"LINE", "@2,3", "@4,0", ""});
    REQUIRE(c.lines().size() == 2);
    CHECK(near(c.lines()[1].a, {12, 8}));
    CHECK(near(c.lines()[1].b, {16, 8}));
}

TEST_CASE("U inside LINE removes the last segment, and the line goes on") {
    Cli c;
    c.say({"LINE", "0,0", "10,0", "10,10", "U", "20,0", ""});
    const auto l = c.lines();
    REQUIRE(l.size() == 2);
    CHECK(near(l[1].b, {20, 0}));
    CHECK(near(l[1].a, {10, 0}));
}

TEST_CASE("the CIRCLE radius is a typed distance") {
    Cli c;
    c.say({"CIRCLE", "0,0", "5"});
    int circles = 0;
    double r = 0;
    c.core->document().forEachEntity([&](const Entity& e) {
        if (const auto* ci = entityAs<CircleData>(e)) {
            ++circles;
            r = ci->radius;
        }
    });
    REQUIRE(circles == 1);
    CHECK(r == doctest::Approx(5));
}

TEST_CASE("a typed command name is not taken as a point or a number inside a prompt") {
    Cli c;
    c.say({"LINE", "0,0", "foo"});
    CHECK(c.running());  // refused with a message, the prompt stays
    c.say({"10,0", ""});
    CHECK(c.lines().size() == 1);
}

TEST_CASE("Escape cancels and Enter at the first prompt of LINE ends it, undo takes it back") {
    Cli c;
    c.say({"LINE", ""});
    CHECK_FALSE(c.running());
    c.say({"LINE", "0,0", "10,0", ""});
    REQUIRE(c.lines().size() == 1);
    c.say({"U"});
    CHECK(c.lines().empty());
}
