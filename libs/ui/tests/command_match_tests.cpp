#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/ui/command_match.h"

using namespace tbcad;
using namespace tbcad::ui;

namespace {

class Named final : public cmd::Command {
public:
    Named(std::string n, std::vector<std::string> a, std::string d)
        : name_(std::move(n)), aliases_(std::move(a)), desc_(std::move(d)) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    std::string_view description() const override { return desc_; }
    cmd::Task run(cmd::CommandContext&) override { co_return; }

private:
    std::string name_;
    std::vector<std::string> aliases_;
    std::string desc_;
};

struct Fx {
    cmd::Registry reg;
    Fx() {
        auto add = [&](const char* n, std::vector<std::string> a, const char* d) {
            REQUIRE(reg.add(std::make_unique<Named>(n, std::move(a), d)));
        };
        add("LINE", {"L"}, "Draw straight segments");
        add("LIMITS", {}, "Set the working area");
        add("CIRCLE", {"C", "CI"}, "Draw a circle");
        add("COPY", {"CO"}, "Copy objects");
        add("POLYLINE", {"PL"}, "Draw connected segments");
        add("FILLET", {"F"}, "Round the corner between two objects");
        add("XLINE", {"XL"}, "Draw an infinite construction line");
    }
};

std::vector<std::string> names(const std::vector<CommandMatch>& v) {
    std::vector<std::string> out;
    for (const auto& m : v)
        out.push_back(m.name);
    return out;
}

}  // namespace

TEST_CASE("exact alias beats alias prefix beats name prefix beats fuzzy") {
    Fx fx;
    auto m = matchCommands(fx.reg, "c");
    REQUIRE(m.size() >= 2);
    CHECK(m[0].name == "CIRCLE");  // exact alias C
    CHECK(m[0].rank == CommandMatch::Rank::ExactAlias);
    CHECK(m[0].alias == "C");
    CHECK(m[1].name == "COPY");  // alias prefix CO (CIRCLE already taken by exact)
    CHECK(m[1].rank == CommandMatch::Rank::AliasPrefix);

    m = matchCommands(fx.reg, "li");
    REQUIRE(m.size() >= 2);
    CHECK(names(m)[0] == "LINE");  // name prefix, the shorter name first among ties
    CHECK(names(m)[1] == "LIMITS");

    m = matchCommands(fx.reg, "cnn");
    CHECK(m.empty());
    m = matchCommands(fx.reg, "plne");
    REQUIRE(m.size() == 1);
    CHECK(m[0].name == "POLYLINE");
    CHECK(m[0].rank == CommandMatch::Rank::Fuzzy);
}

TEST_CASE("synonyms and the middle of a name find commands, as AutoCAD's AutoComplete") {
    Fx fx;
    auto m = matchCommands(fx.reg, "rou");
    REQUIRE_FALSE(m.empty());
    CHECK(m[0].name == "FILLET");
    CHECK(m[0].rank == CommandMatch::Rank::Synonym);
    CHECK(m[0].synonym == "ROUND");
    CHECK(matchCommands(fx.reg, "ro").empty() == false);  // two letters: description words
    // LINE leads, then the names that hold it: XLINE, POLYLINE.
    m = matchCommands(fx.reg, "line");
    REQUIRE(m.size() >= 3);
    CHECK(m[0].name == "LINE");
    CHECK(m[1].rank == CommandMatch::Rank::MidString);
    CHECK(m[1].name == "XLINE");  // the shorter name first
    CHECK(m[2].name == "POLYLINE");
}

TEST_CASE("description words match as the last resort") {
    Fx fx;
    auto m = matchCommands(fx.reg, "straight");
    CHECK(names(m) == std::vector<std::string>{"LINE"});
    CHECK(matchCommands(fx.reg, "q").empty());  // one letter never goes fuzzy
}

TEST_CASE("empty query lists everything, limited") {
    Fx fx;
    CHECK(matchCommands(fx.reg, "").size() == 7);
    CHECK(matchCommands(fx.reg, "", 2).size() == 2);
}

TEST_CASE("case does not matter") {
    Fx fx;
    CHECK(matchCommands(fx.reg, "pl")[0].name == "POLYLINE");
    CHECK(matchCommands(fx.reg, "PL")[0].name == "POLYLINE");
}

TEST_CASE("keyword key length is the shortest unique prefix") {
    std::vector<std::string> k{"Close", "Chord", "Undo"};
    CHECK(keywordKeyLength(k, 0) == 2);
    CHECK(keywordKeyLength(k, 1) == 2);
    CHECK(keywordKeyLength(k, 2) == 1);
    CHECK(keywordKeyLength({"Only"}, 0) == 1);
}

TEST_CASE("commands used lately lead within their rank") {
    Fx fx;
    CHECK(names(matchCommands(fx.reg, "li", 12, {"LIMITS"}))[0] == "LIMITS");
    CHECK(names(matchCommands(fx.reg, "li", 12, {"LIMITS", "LINE"}))[1] == "LINE");
    // A recent command does not climb above a better rank.
    CHECK(matchCommands(fx.reg, "c", 12, {"COPY"})[0].name == "CIRCLE");
}

TEST_CASE("dash commands come after plain ones unless the query starts with a dash") {
    cmd::Registry reg;
    REQUIRE(reg.add(std::make_unique<Named>("-LAYER", std::vector<std::string>{"-LA"}, "Layers")));
    REQUIRE(reg.add(std::make_unique<Named>("LAYER", std::vector<std::string>{"LA"}, "Layers")));
    REQUIRE(reg.add(std::make_unique<Named>("-LAYERP", std::vector<std::string>{}, "x")));
    REQUIRE(reg.add(std::make_unique<Named>("LAYERS", std::vector<std::string>{}, "x")));
    auto m = matchCommands(reg, "lay");
    REQUIRE(m.size() >= 2);
    CHECK(m[0].name == "LAYER");
    CHECK(m[1].name == "LAYERS");
    m = matchCommands(reg, "-lay");
    REQUIRE(!m.empty());
    CHECK(m[0].name[0] == '-');
}
