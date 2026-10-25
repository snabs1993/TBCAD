#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tbcad::lisp {

// A small AutoLISP-style interpreter: dynamic scoping, integers and reals, strings, symbols,
// proper lists and dotted pairs. It knows nothing about drawings; the host adds its own functions
// with Interp::defineSubr (entget, ssget, command ...).

struct Pair;
struct Lambda;
class Interp;
struct Value;

using Args = std::vector<Value>;
using Subr = std::function<Value(Interp&, Args&)>;

struct Value {
    enum class Kind : std::uint8_t { Nil, Int, Real, Str, Sym, Pair, Lambda, Subr, Ename, PickSet };
    Kind kind = Kind::Nil;
    long long i = 0;  // Int; Ename: the entity id
    double r = 0;
    std::string s{};  // Str, Sym (upper case); Subr: name
    std::shared_ptr<Pair> pair{};
    std::shared_ptr<Lambda> fn{};
    std::shared_ptr<std::vector<std::uint64_t>> set{};  // PickSet: entity ids

    bool isNil() const { return kind == Kind::Nil; }
    bool isNumber() const { return kind == Kind::Int || kind == Kind::Real; }
    double number() const { return kind == Kind::Int ? static_cast<double>(i) : r; }

    static Value nil() { return {}; }
    static Value t();
    static Value integer(long long v);
    static Value real(double v);
    static Value str(std::string v);
    static Value sym(std::string v);
    static Value ename(std::uint64_t id);
    static Value pickSet(std::vector<std::uint64_t> ids);
    static Value cons(Value a, Value d);
    static Value list(const std::vector<Value>& items);
    static Value boolean(bool b) { return b ? t() : nil(); }
};

struct Pair {
    Value car{};
    Value cdr{};
};

struct Lambda {
    std::vector<std::string> params{};
    std::vector<std::string> locals{};
    std::vector<Value> body{};
};

// Thrown for every run-time error; `what()` is the AutoLISP-style message ("bad argument type").
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Thrown to stop a running program (Esc at a prompt). Not an error: the host reports "*Cancel*".
struct Abort : std::runtime_error {
    Abort() : std::runtime_error("quit / exit abort") {}
};

// Elements of a proper list (stops at a dotted tail).
std::vector<Value> toVector(const Value& list);
// Printed form: strings in quotes when `quote` is set (prin1), as is otherwise (princ).
std::string toString(const Value& v, bool quote);
// Structural equality as `equal` (reals within fuzz).
bool equalValues(const Value& a, const Value& b, double fuzz = 0);

// AutoLISP wcmatch pattern: * and ? only, case-insensitive.
bool wildcardMatch(std::string_view pattern, std::string_view s);

class Interp {
public:
    Interp();

    // Functions the program can call by name (case-insensitive).
    void defineSubr(std::string name, Subr fn);
    // Sets a global variable.
    void set(std::string_view name, Value v);
    Value get(std::string_view name) const;
    bool isBound(std::string_view name) const;

    // Reads and evaluates every top-level form; returns the last value. Throws Error.
    Value evalText(std::string_view text);
    Value eval(const Value& form);
    // Calls a function value (lambda, subr or a symbol bound to one).
    Value call(const Value& fn, Args args);

    // Output of princ/prin1/print/prompt; defaults to nothing.
    std::function<void(const std::string&)> out{};
    // Reads another source file for (load "name"); returns its text, or nullopt when not found.
    std::function<bool(const std::string&, std::string&)> loader{};
    // Called after (defun c:NAME ...) so the host can make NAME a command.
    std::function<void(const std::string&)> onCommandDefined{};

    // Limits that keep a runaway program from hanging the program.
    std::size_t maxDepth = 160;
    std::uint64_t stepBudget = 50'000'000;  // evaluations per top-level call; 0 = unlimited

    // Called every few thousand evaluations; throw Abort from it to stop the program.
    std::function<void()> poll{};

private:
    friend struct SpecialForms;
    Value evalList(const Value& form);
    Value apply(const Value& fn, Args& args);
    Value callLambda(const Lambda& fn, Args& args);

    std::unordered_map<std::string, Value> globals_{};
    std::unordered_map<std::string, Subr> subrs_{};
    std::size_t depth_ = 0;
    std::uint64_t steps_ = 0;
};

// Parses source text into top-level forms. Throws Error on unbalanced input.
std::vector<Value> readAll(std::string_view text);

}  // namespace tbcad::lisp
