#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

class Fake final : public Command {
public:
    Fake(std::string n, std::vector<std::string> a, std::string d)
        : name_(std::move(n)), aliases_(std::move(a)), desc_(std::move(d)) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    std::string_view description() const override { return desc_; }
    Task run(CommandContext&) override { co_return; }

private:
    std::string name_;
    std::vector<std::string> aliases_;
    std::string desc_;
};

}  // namespace

TEST_CASE("the command list is a Markdown table without hidden commands") {
    Registry reg;
    (void)reg.add(std::make_unique<Fake>("LINE", std::vector<std::string>{"L"}, "Draw lines"));
    (void)reg.add(std::make_unique<Fake>("CIRCLE", std::vector<std::string>{"C", "CI"},
                                         "Circle | by centre"));
    (void)reg.add(std::make_unique<Fake>("HIDDEN", std::vector<std::string>{"HX"}, ""));
    reg.setUserAliases({{"CC", "CIRCLE"}});
    const std::string md = commandListMarkdown(reg);
    CHECK(md.find("| Command | Aliases | What it does |") == 0);
    CHECK(md.find("| LINE | L | Draw lines |") != std::string::npos);
    CHECK(md.find("| CIRCLE | C, CC, CI | Circle \\| by centre |") != std::string::npos);
    CHECK(md.find("HIDDEN") == std::string::npos);
    // Sorted by name: CIRCLE before LINE.
    CHECK(md.find("CIRCLE") < md.find("LINE"));
}
