#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::expr {

// A formula value: a number or a piece of text. Comparisons give 1 or 0.
struct Value {
    bool isText = false;
    double number = 0;
    std::string text{};

    static Value ofNumber(double n) { return {false, n, {}}; }
    static Value ofText(std::string t) { return {true, 0, std::move(t)}; }
    // Numbers print in the shortest form that reads back (3, 2.5); text as it is.
    std::string toString() const;
    // Text that reads as a number counts as that number; other text and empty text are 0.
    double toNumber() const;
};

// Looks up a parameter by name; empty when there is no such parameter.
using Env = std::function<std::optional<Value>(std::string_view name)>;

// A parsed formula.
//   literals   12  2.5  1e3  "text"           names      Length  Width_2  (letters, digits, _,
//   Thai) operators  ^  unary -  * /  + -  &(text join)  < <= > >= == !=  &&  ||  ! functions  min
//   max abs round floor ceil sqrt pow sin cos tan len if(cond, a, b) constants  pi
// Division by zero, an unknown name or function, or text where a number is needed is an error
// that names the problem.
class Expression {
public:
    static Result<Expression> parse(std::string_view source);

    Result<Value> eval(const Env& env) const;
    // The parameter names the formula refers to (without constants), each once.
    std::vector<std::string> names() const;
    const std::string& source() const { return source_; }

    struct Node;

private:
    std::string source_{};
    std::shared_ptr<const Node> root_{};
};

}  // namespace tbcad::expr
