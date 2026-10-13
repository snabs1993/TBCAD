#include "tbcad/expr/expr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tbcad::expr {

std::string Value::toString() const {
    if (isText)
        return text;
    if (std::isfinite(number) && number == std::floor(number) && std::abs(number) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", number);
        return buf;
    }
    char buf[40];
    for (int digits = 1; digits <= 17; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, number);
        if (std::strtod(buf, nullptr) == number)
            break;
    }
    return buf;
}

double Value::toNumber() const {
    if (!isText)
        return number;
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' ? v : 0.0;
}

struct Expression::Node {
    enum class Kind { Number, Text, Name, Unary, Binary, Call };
    Kind kind = Kind::Number;
    double number = 0;
    std::string text{};  // text literal, name, operator or function name
    std::vector<std::shared_ptr<const Node>> args{};
};

namespace {

using NodePtr = std::shared_ptr<const Expression::Node>;

bool isNameStart(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}
bool isNameChar(unsigned char c) {
    return isNameStart(c) || std::isdigit(c);
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<NodePtr> run() {
        auto r = orExpr();
        if (!r)
            return r;
        skip();
        if (pos_ != s_.size())
            return fail("unexpected \"" + std::string(1, s_[pos_]) + "\"");
        return r;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
    int depth_ = 0;

    Error fail(const std::string& m) const {
        return Error{ErrorCode::Parse, m + " (at position " + std::to_string(pos_ + 1) + ")"};
    }
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t'))
            ++pos_;
    }
    bool take(std::string_view op) {
        skip();
        if (s_.substr(pos_, op.size()) == op) {
            pos_ += op.size();
            return true;
        }
        return false;
    }
    static NodePtr make(Expression::Node::Kind k, std::string text, double n = 0,
                        std::vector<NodePtr> args = {}) {
        auto node = std::make_shared<Expression::Node>();
        node->kind = k;
        node->text = std::move(text);
        node->number = n;
        node->args = std::move(args);
        return node;
    }

    Result<NodePtr> binary(std::string op, NodePtr a, NodePtr b) {
        return make(Expression::Node::Kind::Binary, std::move(op), 0, {std::move(a), std::move(b)});
    }

    Result<NodePtr> orExpr() {
        auto l = andExpr();
        while (l && take("||")) {
            auto r = andExpr();
            if (!r)
                return r;
            l = binary("||", l.value(), r.value());
        }
        return l;
    }
    Result<NodePtr> andExpr() {
        auto l = compare();
        while (l && take("&&")) {
            auto r = compare();
            if (!r)
                return r;
            l = binary("&&", l.value(), r.value());
        }
        return l;
    }
    Result<NodePtr> compare() {
        auto l = join();
        while (l) {
            std::string op;
            for (const char* c : {"<=", ">=", "==", "!=", "<", ">"})
                if (op.empty() && take(c))
                    op = c;
            if (op.empty())
                break;
            auto r = join();
            if (!r)
                return r;
            l = binary(op, l.value(), r.value());
        }
        return l;
    }
    Result<NodePtr> join() {
        auto l = additive();
        while (l) {
            skip();
            // "&&" belongs to the logical operator
            if (pos_ < s_.size() && s_[pos_] == '&' && s_.substr(pos_, 2) != "&&") {
                ++pos_;
                auto r = additive();
                if (!r)
                    return r;
                l = binary("&", l.value(), r.value());
            } else {
                break;
            }
        }
        return l;
    }
    Result<NodePtr> additive() {
        auto l = term();
        while (l) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                std::string op(1, s_[pos_++]);
                auto r = term();
                if (!r)
                    return r;
                l = binary(op, l.value(), r.value());
            } else {
                break;
            }
        }
        return l;
    }
    Result<NodePtr> term() {
        auto l = unary();
        while (l) {
            skip();
            if (pos_ < s_.size() && (s_[pos_] == '*' || s_[pos_] == '/')) {
                std::string op(1, s_[pos_++]);
                auto r = unary();
                if (!r)
                    return r;
                l = binary(op, l.value(), r.value());
            } else {
                break;
            }
        }
        return l;
    }
    Result<NodePtr> unary() {
        skip();
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '!' || s_[pos_] == '+')) {
            char op = s_[pos_];
            if (!(op == '!' && s_.substr(pos_, 2) == "!=")) {
                ++pos_;
                auto r = unary();
                if (!r)
                    return r;
                if (op == '+')
                    return r;
                return make(Expression::Node::Kind::Unary, std::string(1, op), 0, {r.value()});
            }
        }
        return power();
    }
    Result<NodePtr> power() {
        auto base = primary();
        if (!base)
            return base;
        if (take("^")) {
            auto e = unary();  // right associative, binds tighter than unary minus on the left
            if (!e)
                return e;
            return binary("^", base.value(), e.value());
        }
        return base;
    }
    Result<NodePtr> primary() {
        skip();
        if (pos_ >= s_.size())
            return fail("the formula ends too early");
        if (++depth_ > 64)
            return fail("the formula is nested too deeply");
        struct Leave {
            int& d;
            ~Leave() { --d; }
        } leave{depth_};
        unsigned char c = static_cast<unsigned char>(s_[pos_]);
        if (c == '(') {
            ++pos_;
            auto r = orExpr();
            if (!r)
                return r;
            if (!take(")"))
                return fail("expected )");
            return r;
        }
        if (std::isdigit(c) || c == '.') {
            const char* start = s_.data() + pos_;
            char* end = nullptr;
            double v = std::strtod(start, &end);
            if (end == start)
                return fail("bad number");
            pos_ += static_cast<std::size_t>(end - start);
            return make(Expression::Node::Kind::Number, {}, v);
        }
        if (c == '"') {
            std::string text;
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] != '"')
                text += s_[pos_++];
            if (pos_ >= s_.size())
                return fail("text is missing its closing quote");
            ++pos_;
            return make(Expression::Node::Kind::Text, std::move(text));
        }
        if (isNameStart(c)) {
            std::size_t start = pos_;
            while (pos_ < s_.size() && isNameChar(static_cast<unsigned char>(s_[pos_])))
                ++pos_;
            std::string name(s_.substr(start, pos_ - start));
            skip();
            if (pos_ < s_.size() && s_[pos_] == '(') {
                ++pos_;
                std::vector<NodePtr> args;
                if (!take(")")) {
                    for (;;) {
                        auto a = orExpr();
                        if (!a)
                            return a;
                        args.push_back(a.value());
                        if (take(")"))
                            break;
                        if (!take(","))
                            return fail("expected , or )");
                    }
                }
                return make(Expression::Node::Kind::Call, std::move(name), 0, std::move(args));
            }
            return make(Expression::Node::Kind::Name, std::move(name));
        }
        return fail("unexpected \"" + std::string(1, s_[pos_]) + "\"");
    }
};

void collectNames(const Expression::Node& n, std::vector<std::string>& out) {
    if (n.kind == Expression::Node::Kind::Name && n.text != "pi" &&
        std::find(out.begin(), out.end(), n.text) == out.end())
        out.push_back(n.text);
    for (const auto& a : n.args)
        collectNames(*a, out);
}

Result<double> number(const Value& v, const std::string& what) {
    if (v.isText) {
        char* end = nullptr;
        double d = std::strtod(v.text.c_str(), &end);
        if (end == v.text.c_str() || *end != '\0')
            return makeError(ErrorCode::Parse, "\"" + v.text + "\" is text, but " + what + " needs a number");
        return d;
    }
    return v.number;
}

Result<Value> evalNode(const Expression::Node& n, const Env& env, int depth) {
    using K = Expression::Node::Kind;
    if (depth > 200)
        return makeError(ErrorCode::Parse, "the formula is nested too deeply");
    switch (n.kind) {
        case K::Number: return Value::ofNumber(n.number);
        case K::Text: return Value::ofText(n.text);
        case K::Name: {
            if (n.text == "pi")
                return Value::ofNumber(3.14159265358979323846);
            auto v = env ? env(n.text) : std::nullopt;
            if (!v)
                return makeError(ErrorCode::NotFound, "unknown name \"" + n.text + "\"");
            return *v;
        }
        case K::Unary: {
            auto a = evalNode(*n.args[0], env, depth + 1);
            if (!a)
                return a;
            if (n.text == "!")
                return Value::ofNumber(a.value().toNumber() == 0 ? 1 : 0);
            auto d = number(a.value(), "-");
            if (!d)
                return d.error();
            return Value::ofNumber(-d.value());
        }
        case K::Binary: {
            if (n.text == "&&" || n.text == "||") {
                auto l = evalNode(*n.args[0], env, depth + 1);
                if (!l)
                    return l;
                const bool lt = l.value().isText ? !l.value().text.empty() : l.value().number != 0;
                if (n.text == "&&" && !lt)
                    return Value::ofNumber(0);
                if (n.text == "||" && lt)
                    return Value::ofNumber(1);
                auto r = evalNode(*n.args[1], env, depth + 1);
                if (!r)
                    return r;
                const bool rt = r.value().isText ? !r.value().text.empty() : r.value().number != 0;
                return Value::ofNumber(rt ? 1 : 0);
            }
            auto l = evalNode(*n.args[0], env, depth + 1);
            if (!l)
                return l;
            auto r = evalNode(*n.args[1], env, depth + 1);
            if (!r)
                return r;
            const Value& a = l.value();
            const Value& b = r.value();
            if (n.text == "&")
                return Value::ofText(a.toString() + b.toString());
            if (n.text == "==" || n.text == "!=") {
                bool eq = (a.isText || b.isText) ? a.toString() == b.toString() : a.number == b.number;
                return Value::ofNumber((n.text == "==") == eq ? 1 : 0);
            }
            auto x = number(a, n.text);
            if (!x)
                return x.error();
            auto y = number(b, n.text);
            if (!y)
                return y.error();
            const double p = x.value(), q = y.value();
            if (n.text == "+") return Value::ofNumber(p + q);
            if (n.text == "-") return Value::ofNumber(p - q);
            if (n.text == "*") return Value::ofNumber(p * q);
            if (n.text == "/") {
                if (q == 0)
                    return makeError(ErrorCode::OutOfRange, "division by zero");
                return Value::ofNumber(p / q);
            }
            if (n.text == "^") return Value::ofNumber(std::pow(p, q));
            if (n.text == "<") return Value::ofNumber(p < q);
            if (n.text == "<=") return Value::ofNumber(p <= q);
            if (n.text == ">") return Value::ofNumber(p > q);
            if (n.text == ">=") return Value::ofNumber(p >= q);
            return makeError(ErrorCode::Parse, "unknown operator " + n.text);
        }
        case K::Call: {
            const std::string& f = n.text;
            if (f == "if") {
                if (n.args.size() != 3)
                    return makeError(ErrorCode::Parse, "if needs 3 arguments");
                auto c = evalNode(*n.args[0], env, depth + 1);
                if (!c)
                    return c;
                const bool t = c.value().isText ? !c.value().text.empty() : c.value().number != 0;
                return evalNode(*n.args[t ? 1 : 2], env, depth + 1);
            }
            std::vector<Value> v;
            for (const auto& a : n.args) {
                auto r = evalNode(*a, env, depth + 1);
                if (!r)
                    return r;
                v.push_back(std::move(r).value());
            }
            if (f == "len") {
                if (v.size() != 1)
                    return makeError(ErrorCode::Parse, "len needs 1 argument");
                return Value::ofNumber(static_cast<double>(v[0].toString().size()));
            }
            std::vector<double> d;
            for (const Value& x : v) {
                auto r = number(x, f);
                if (!r)
                    return r.error();
                d.push_back(r.value());
            }
            auto need = [&](std::size_t k) -> std::optional<Error> {
                if (d.size() != k)
                    return Error{ErrorCode::Parse, f + " needs " + std::to_string(k) + " argument(s)"};
                return std::nullopt;
            };
            if (f == "min" || f == "max") {
                if (d.empty())
                    return makeError(ErrorCode::Parse, f + " needs at least 1 argument");
                return Value::ofNumber(f == "min" ? *std::min_element(d.begin(), d.end())
                                                  : *std::max_element(d.begin(), d.end()));
            }
            if (f == "pow") {
                if (auto e = need(2)) return *e;
                return Value::ofNumber(std::pow(d[0], d[1]));
            }
            if (f == "round" && d.size() == 2) {
                const double k = std::pow(10.0, d[1]);
                return Value::ofNumber(std::round(d[0] * k) / k);
            }
            struct Fn { const char* name; double (*fn)(double); };
            static const Fn fns[] = {{"abs", [](double x) { return std::abs(x); }},
                                     {"round", [](double x) { return std::round(x); }},
                                     {"floor", [](double x) { return std::floor(x); }},
                                     {"ceil", [](double x) { return std::ceil(x); }},
                                     {"sqrt", [](double x) { return std::sqrt(x); }},
                                     {"sin", [](double x) { return std::sin(x); }},
                                     {"cos", [](double x) { return std::cos(x); }},
                                     {"tan", [](double x) { return std::tan(x); }}};
            for (const Fn& fn : fns)
                if (f == fn.name) {
                    if (auto e = need(1)) return *e;
                    if (f == "sqrt" && d[0] < 0)
                        return makeError(ErrorCode::OutOfRange, "sqrt of a negative number");
                    return Value::ofNumber(fn.fn(d[0]));
                }
            return makeError(ErrorCode::NotFound, "unknown function \"" + f + "\"");
        }
    }
    return makeError(ErrorCode::Parse, "bad formula");
}

}  // namespace

Result<Expression> Expression::parse(std::string_view source) {
    Parser p(source);
    auto root = p.run();
    if (!root)
        return root.error();
    Expression e;
    e.source_ = std::string(source);
    e.root_ = std::move(root).value();
    return e;
}

Result<Value> Expression::eval(const Env& env) const {
    if (!root_)
        return makeError(ErrorCode::Parse, "empty formula");
    auto r = evalNode(*root_, env, 0);
    if (r && !r.value().isText && !std::isfinite(r.value().number))
        return makeError(ErrorCode::OutOfRange, "the result is not a finite number");
    return r;
}

std::vector<std::string> Expression::names() const {
    std::vector<std::string> out;
    if (root_)
        collectNames(*root_, out);
    return out;
}

}  // namespace tbcad::expr
