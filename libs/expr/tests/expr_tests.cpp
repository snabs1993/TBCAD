#include <doctest/doctest.h>

#include <map>
#include <string>
#include <string_view>

#include "tbcad/expr/expr.h"

using namespace tbcad;
using namespace tbcad::expr;

namespace {

Result<Value> run(const char* src, std::map<std::string, Value> vars = {}) {
    auto e = Expression::parse(src);
    if (!e)
        return e.error();
    return e.value().eval([&](std::string_view n) -> std::optional<Value> {
        auto it = vars.find(std::string(n));
        if (it == vars.end())
            return std::nullopt;
        return it->second;
    });
}

double num(const char* src, std::map<std::string, Value> vars = {}) {
    auto r = run(src, std::move(vars));
    REQUIRE(r.ok());
    return r.value().toNumber();
}

}  // namespace

TEST_CASE("expr: arithmetic, precedence and power") {
    CHECK(num("1 + 2 * 3") == 7);
    CHECK(num("(1 + 2) * 3") == 9);
    CHECK(num("2 ^ 3 ^ 2") == 512);
    CHECK(num("-2 ^ 2") == -4);
    CHECK(num("2 ^ -1") == 0.5);
    CHECK(num("10 / 4") == 2.5);
    CHECK(num("1e3 + .5") == 1000.5);
    CHECK(num("pi") == doctest::Approx(3.14159265));
}

TEST_CASE("expr: names, Thai names and unknown names") {
    CHECK(num("Length * 2 + Width",
              {{"Length", Value::ofNumber(5)}, {"Width", Value::ofNumber(1)}}) == 11);
    CHECK(num("ความยาว / 2", {{"ความยาว", Value::ofNumber(8)}}) == 4);
    auto r = run("Nope + 1");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("Nope") != std::string::npos);
}

TEST_CASE("expr: comparison, logic and if") {
    CHECK(num("3 > 2") == 1);
    CHECK(num("3 <= 2") == 0);
    CHECK(num("2 == 2 && 1 != 2") == 1);
    CHECK(num("0 || 0") == 0);
    CHECK(num("!0") == 1);
    CHECK(num("if(Legs > 4, 10, 20)", {{"Legs", Value::ofNumber(6)}}) == 10);
    CHECK(num("if(Legs > 4, 10, 20)", {{"Legs", Value::ofNumber(2)}}) == 20);
    CHECK(num("if(1, 5, 1/0)") == 5);  // only the chosen branch is evaluated
}

TEST_CASE("expr: functions") {
    CHECK(num("min(3, 1, 2)") == 1);
    CHECK(num("max(3, 1, 2)") == 3);
    CHECK(num("abs(-4)") == 4);
    CHECK(num("round(2.567, 2)") == doctest::Approx(2.57));
    CHECK(num("floor(2.9) + ceil(2.1)") == 5);
    CHECK(num("sqrt(16)") == 4);
    CHECK(num("pow(2, 10)") == 1024);
    CHECK(num("len(\"abc\")") == 3);
    CHECK_FALSE(run("sqrt(-1)").ok());
    CHECK_FALSE(run("nosuch(1)").ok());
    CHECK_FALSE(run("min()").ok());
}

TEST_CASE("expr: text values and joining") {
    auto r = run("\"Table \" & Length & \" mm\"", {{"Length", Value::ofNumber(1200)}});
    REQUIRE(r.ok());
    CHECK(r.value().isText);
    CHECK(r.value().text == "Table 1200 mm");
    CHECK(num("Material == \"Oak\"", {{"Material", Value::ofText("Oak")}}) == 1);
    CHECK(num("Count + 1", {{"Count", Value::ofText("4")}}) == 5);  // numeric text counts
    CHECK_FALSE(run("Label + 1", {{"Label", Value::ofText("abc")}}).ok());
    CHECK(Value::ofNumber(2.5).toString() == "2.5");
    CHECK(Value::ofNumber(3).toString() == "3");
}

TEST_CASE("expr: errors give a position and never crash") {
    CHECK_FALSE(Expression::parse("1 +").ok());
    CHECK_FALSE(Expression::parse("(1").ok());
    CHECK_FALSE(Expression::parse("1 2").ok());
    CHECK_FALSE(Expression::parse("\"open").ok());
    CHECK_FALSE(Expression::parse("1 $ 2").ok());
    CHECK_FALSE(run("1 / 0").ok());
    CHECK_FALSE(Expression::parse(std::string(500, '(') + "1" + std::string(500, ')')).ok());
}

TEST_CASE("expr: names() lists each parameter once") {
    auto e = Expression::parse("A * 2 + A + max(B, pi)");
    REQUIRE(e.ok());
    auto names = e.value().names();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "A");
    CHECK(names[1] == "B");
}

TEST_CASE("expr: hostile formulas give errors, not crashes (found by fuzzing)") {
    // A number at the end of a view that is not NUL-terminated was read past its end.
    const std::string whole = "12345";
    auto e = Expression::parse(std::string_view(whole).substr(0, 2));
    REQUIRE(e.ok());
    auto v = e.value().eval([](std::string_view) { return std::nullopt; });
    REQUIRE(v.ok());
    CHECK(v.value().number == 12);
    CHECK(num("1.5e-3*2") == doctest::Approx(0.003));
    CHECK(num("2-1") == 1);
    // Signs nest like brackets: deep enough fails cleanly instead of overflowing the stack.
    CHECK(num("--3") == 3);
    CHECK_FALSE(run(std::string(100000, '-').append("1").c_str()).ok());
    CHECK_FALSE(run(std::string(100000, '!').append("1").c_str()).ok());
}
