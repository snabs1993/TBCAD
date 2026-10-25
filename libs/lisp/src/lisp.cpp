#include "tbcad/lisp/lisp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>

namespace tbcad::lisp {

// ---- values ------------------------------------------------------------------------------------

Value Value::t() {
    Value v;
    v.kind = Kind::Sym;
    v.s = "T";
    return v;
}
Value Value::integer(long long x) {
    Value v;
    v.kind = Kind::Int;
    v.i = x;
    return v;
}
Value Value::real(double x) {
    Value v;
    v.kind = Kind::Real;
    v.r = x;
    return v;
}
Value Value::str(std::string x) {
    Value v;
    v.kind = Kind::Str;
    v.s = std::move(x);
    return v;
}
Value Value::sym(std::string x) {
    Value v;
    v.kind = Kind::Sym;
    for (auto& c : x)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    v.s = std::move(x);
    if (v.s == "NIL")
        return {};
    return v;
}
Value Value::ename(std::uint64_t id) {
    Value v;
    v.kind = Kind::Ename;
    v.i = static_cast<long long>(id);
    return v;
}
Value Value::pickSet(std::vector<std::uint64_t> ids) {
    Value v;
    v.kind = Kind::PickSet;
    v.set = std::make_shared<std::vector<std::uint64_t>>(std::move(ids));
    return v;
}
Value Value::cons(Value a, Value d) {
    Value v;
    v.kind = Kind::Pair;
    v.pair = std::make_shared<Pair>(Pair{std::move(a), std::move(d)});
    return v;
}
Value Value::list(const std::vector<Value>& items) {
    Value out;
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        out = cons(*it, std::move(out));
    return out;
}

std::vector<Value> toVector(const Value& list) {
    std::vector<Value> out;
    const Value* p = &list;
    while (p->kind == Value::Kind::Pair) {
        out.push_back(p->pair->car);
        p = &p->pair->cdr;
    }
    return out;
}

namespace {

std::string realText(double v) {
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s += '0';
    if (s == "-0.0")
        s = "0.0";
    return s;
}

void print(const Value& v, bool quote, std::string& out) {
    switch (v.kind) {
        case Value::Kind::Nil:
            out += "nil";
            break;
        case Value::Kind::Int:
            out += std::to_string(v.i);
            break;
        case Value::Kind::Real:
            out += realText(v.r);
            break;
        case Value::Kind::Str:
            if (!quote) {
                out += v.s;
                break;
            }
            out += '"';
            for (char c : v.s) {
                if (c == '"' || c == '\\')
                    out += '\\';
                if (c == '\n') {
                    out += "\\n";
                    continue;
                }
                out += c;
            }
            out += '"';
            break;
        case Value::Kind::Sym:
            out += v.s;
            break;
        case Value::Kind::Pair: {
            out += '(';
            const Value* p = &v;
            bool first = true;
            std::size_t guard = 0;
            while (p->kind == Value::Kind::Pair && guard++ < 100000) {
                if (!first)
                    out += ' ';
                first = false;
                print(p->pair->car, quote, out);
                p = &p->pair->cdr;
            }
            if (!p->isNil()) {
                out += " . ";
                print(*p, quote, out);
            }
            out += ')';
            break;
        }
        case Value::Kind::Lambda:
            out += "#<USUBR>";
            break;
        case Value::Kind::Subr:
            out += "#<SUBR @" + v.s + ">";
            break;
        case Value::Kind::Ename:
            out += "<Entity name: " + std::to_string(v.i) + ">";
            break;
        case Value::Kind::PickSet:
            out += "<Selection set: " + std::to_string(v.set ? v.set->size() : 0) + ">";
            break;
    }
}

}  // namespace

std::string toString(const Value& v, bool quote) {
    std::string out;
    print(v, quote, out);
    return out;
}

bool equalValues(const Value& a, const Value& b, double fuzz) {
    using K = Value::Kind;
    if (a.isNumber() && b.isNumber()) {
        if (a.kind == K::Int && b.kind == K::Int)
            return a.i == b.i;
        return std::fabs(a.number() - b.number()) <= fuzz;
    }
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case K::Nil:
            return true;
        case K::Str:
        case K::Sym:
        case K::Subr:
            return a.s == b.s;
        case K::Ename:
            return a.i == b.i;
        case K::PickSet:
            return a.set == b.set;
        case K::Lambda:
            return a.fn == b.fn;
        case K::Pair: {
            const Value* x = &a;
            const Value* y = &b;
            while (x->kind == K::Pair && y->kind == K::Pair) {
                if (!equalValues(x->pair->car, y->pair->car, fuzz))
                    return false;
                x = &x->pair->cdr;
                y = &y->pair->cdr;
            }
            return equalValues(*x, *y, fuzz);
        }
        default:
            return false;
    }
}

// ---- reader
// --------------------------------------------------------------------------------------

namespace {

class Reader {
public:
    explicit Reader(std::string_view t) : t_(t) {}

    std::vector<Value> all() {
        std::vector<Value> out;
        for (;;) {
            skip();
            if (pos_ >= t_.size())
                return out;
            out.push_back(read(0));
        }
    }

private:
    void skip() {
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == ';') {
                if (pos_ + 1 < t_.size() && t_[pos_ + 1] == '|') {
                    auto end = t_.find("|;", pos_ + 2);
                    pos_ = end == std::string_view::npos ? t_.size() : end + 2;
                } else {
                    while (pos_ < t_.size() && t_[pos_] != '\n')
                        ++pos_;
                }
            } else {
                break;
            }
        }
    }

    Value read(int depth) {
        if (depth > 200)
            throw Error("nesting too deep");
        skip();
        if (pos_ >= t_.size())
            throw Error("malformed list on input");
        char c = t_[pos_];
        if (c == '(')
            return readList(depth);
        if (c == ')')
            throw Error("extra right paren on input");
        if (c == '\'') {
            ++pos_;
            return Value::list({Value::sym("QUOTE"), read(depth + 1)});
        }
        if (c == '"')
            return readString();
        return readAtom();
    }

    Value readList(int depth) {
        ++pos_;  // (
        std::vector<Value> items;
        Value tail;
        for (;;) {
            skip();
            if (pos_ >= t_.size())
                throw Error("malformed list on input");
            if (t_[pos_] == ')') {
                ++pos_;
                break;
            }
            if (t_[pos_] == '.' && pos_ + 1 < t_.size() &&
                (std::isspace(static_cast<unsigned char>(t_[pos_ + 1])) || t_[pos_ + 1] == '(') &&
                !items.empty()) {
                ++pos_;
                tail = read(depth + 1);
                skip();
                if (pos_ >= t_.size() || t_[pos_] != ')')
                    throw Error("malformed list on input");
                ++pos_;
                break;
            }
            items.push_back(read(depth + 1));
        }
        Value out = tail;
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            out = Value::cons(std::move(*it), std::move(out));
        return out;
    }

    Value readString() {
        ++pos_;
        std::string s;
        while (pos_ < t_.size() && t_[pos_] != '"') {
            char c = t_[pos_++];
            if (c == '\\' && pos_ < t_.size()) {
                char n = t_[pos_++];
                switch (n) {
                    case 'n':
                        s += '\n';
                        break;
                    case 't':
                        s += '\t';
                        break;
                    case 'r':
                        s += '\r';
                        break;
                    case 'e':
                        s += '\x1b';
                        break;
                    default:
                        s += n;
                }
            } else {
                s += c;
            }
        }
        if (pos_ >= t_.size())
            throw Error("malformed string on input");
        ++pos_;
        return Value::str(std::move(s));
    }

    Value readAtom() {
        std::size_t start = pos_;
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '\'' ||
                c == '"' || c == ';')
                break;
            ++pos_;
        }
        std::string tok(t_.substr(start, pos_ - start));
        if (tok.empty())
            throw Error("bad input");
        // Number?
        char* end = nullptr;
        bool numeric = std::isdigit(static_cast<unsigned char>(tok[0])) != 0 ||
                       ((tok[0] == '-' || tok[0] == '+' || tok[0] == '.') && tok.size() > 1 &&
                        (std::isdigit(static_cast<unsigned char>(tok[1])) != 0 || tok[1] == '.'));
        if (numeric) {
            bool isInt = tok.find_first_of(".eE") == std::string::npos;
            if (isInt) {
                long long v = std::strtoll(tok.c_str(), &end, 10);
                if (end && *end == '\0')
                    return Value::integer(v);
            } else {
                double v = std::strtod(tok.c_str(), &end);
                if (end && *end == '\0')
                    return Value::real(v);
            }
        }
        return Value::sym(tok);
    }

    std::string_view t_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<Value> readAll(std::string_view text) {
    return Reader(text).all();
}

// ---- evaluator
// -----------------------------------------------------------------------------------

namespace {

using K = Value::Kind;

[[noreturn]] void bad(const char* what = "bad argument type") {
    throw Error(what);
}

const Value& argAt(const Args& a, std::size_t i) {
    static const Value nilValue;
    return i < a.size() ? a[i] : nilValue;
}
void needArgs(const Args& a, std::size_t n) {
    if (a.size() < n)
        throw Error("too few arguments");
}
double num(const Value& v) {
    if (!v.isNumber())
        bad();
    return v.number();
}
const std::string& text(const Value& v) {
    if (v.kind != K::Str)
        bad();
    return v.s;
}
std::vector<double> coords(const Value& v) {
    std::vector<double> c;
    for (const auto& e : toVector(v))
        c.push_back(num(e));
    if (c.size() < 2)
        bad();
    return c;
}
Value pointValue(double x, double y, double z) {
    return Value::list({Value::real(x), Value::real(y), Value::real(z)});
}

}  // namespace

bool wildcardMatch(std::string_view pat, std::string_view s) {
    if (pat.empty())
        return s.empty();
    if (pat[0] == '*') {
        for (std::size_t k = 0; k <= s.size(); ++k)
            if (wildcardMatch(pat.substr(1), s.substr(k)))
                return true;
        return false;
    }
    if (s.empty())
        return false;
    if (pat[0] == '?' || std::toupper(static_cast<unsigned char>(pat[0])) ==
                             std::toupper(static_cast<unsigned char>(s[0])))
        return wildcardMatch(pat.substr(1), s.substr(1));
    return false;
}

namespace {

std::string upper(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string rtosText(double v, int mode, int prec) {
    prec = std::clamp(prec, 0, 16);
    char buf[128];
    if (mode == 1) {
        std::snprintf(buf, sizeof buf, "%.*E", prec, v);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    std::string s = buf;
    if (mode == 2 && s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

}  // namespace

struct SpecialForms {
    using Fn = Value (*)(Interp&, const Value& rest);
    static const std::unordered_map<std::string, Fn>& table();
};

Interp::Interp() {
    using A = Args;
    auto def = [this](const char* n, Subr f) { defineSubr(n, std::move(f)); };
    globals_["PI"] = Value::real(std::numbers::pi);

    // ---- arithmetic ----
    def("+", [](Interp&, A& a) {
        bool real = false;
        double s = 0;
        long long si = 0;
        for (auto& v : a) {
            if (!v.isNumber())
                bad();
            if (v.kind == K::Real)
                real = true;
            s += v.number();
            if (v.kind == K::Int)
                si += v.i;
        }
        return real ? Value::real(s) : Value::integer(si);
    });
    def("*", [](Interp&, A& a) {
        bool real = false;
        double s = 1;
        long long si = 1;
        for (auto& v : a) {
            if (!v.isNumber())
                bad();
            if (v.kind == K::Real)
                real = true;
            s *= v.number();
            if (v.kind == K::Int)
                si *= v.i;
        }
        return real ? Value::real(s) : Value::integer(si);
    });
    def("-", [](Interp&, A& a) {
        needArgs(a, 1);
        bool real = false;
        for (auto& v : a) {
            if (!v.isNumber())
                bad();
            real = real || v.kind == K::Real;
        }
        if (a.size() == 1)
            return real ? Value::real(-a[0].number()) : Value::integer(-a[0].i);
        double s = a[0].number();
        long long si = a[0].i;
        for (std::size_t k = 1; k < a.size(); ++k) {
            s -= a[k].number();
            si -= a[k].i;
        }
        return real ? Value::real(s) : Value::integer(si);
    });
    def("/", [](Interp&, A& a) {
        needArgs(a, 1);
        bool real = false;
        for (auto& v : a) {
            if (!v.isNumber())
                bad();
            real = real || v.kind == K::Real;
        }
        if (a.size() == 1)
            a.insert(a.begin(), Value::integer(1));
        double s = a[0].number();
        long long si = a[0].i;
        for (std::size_t k = 1; k < a.size(); ++k) {
            if (a[k].number() == 0)
                throw Error("divide by zero");
            s /= a[k].number();
            if (!real)
                si /= a[k].i;
        }
        return real ? Value::real(s) : Value::integer(si);
    });
    def("1+", [](Interp&, A& a) {
        needArgs(a, 1);
        return a[0].kind == K::Int ? Value::integer(a[0].i + 1) : Value::real(num(a[0]) + 1);
    });
    def("1-", [](Interp&, A& a) {
        needArgs(a, 1);
        return a[0].kind == K::Int ? Value::integer(a[0].i - 1) : Value::real(num(a[0]) - 1);
    });
    def("REM", [](Interp&, A& a) {
        needArgs(a, 2);
        if (a[0].kind == K::Int && a[1].kind == K::Int) {
            if (a[1].i == 0)
                throw Error("divide by zero");
            return Value::integer(a[0].i % a[1].i);
        }
        if (num(a[1]) == 0)
            throw Error("divide by zero");
        return Value::real(std::fmod(num(a[0]), num(a[1])));
    });
    def("ABS", [](Interp&, A& a) {
        needArgs(a, 1);
        return a[0].kind == K::Int ? Value::integer(std::llabs(a[0].i))
                                   : Value::real(std::fabs(num(a[0])));
    });
    def("MAX", [](Interp&, A& a) {
        needArgs(a, 1);
        bool real = false;
        double m = num(a[0]);
        for (auto& v : a) {
            real = real || v.kind == K::Real;
            m = std::max(m, num(v));
        }
        return real ? Value::real(m) : Value::integer(static_cast<long long>(m));
    });
    def("MIN", [](Interp&, A& a) {
        needArgs(a, 1);
        bool real = false;
        double m = num(a[0]);
        for (auto& v : a) {
            real = real || v.kind == K::Real;
            m = std::min(m, num(v));
        }
        return real ? Value::real(m) : Value::integer(static_cast<long long>(m));
    });
    def("SQRT", [](Interp&, A& a) {
        needArgs(a, 1);
        double v = num(a[0]);
        if (v < 0)
            throw Error("function undefined for arguments");
        return Value::real(std::sqrt(v));
    });
    def("SIN", [](Interp&, A& a) { return Value::real(std::sin(num(argAt(a, 0)))); });
    def("COS", [](Interp&, A& a) { return Value::real(std::cos(num(argAt(a, 0)))); });
    def("ATAN", [](Interp&, A& a) {
        needArgs(a, 1);
        return Value::real(a.size() > 1 ? std::atan2(num(a[0]), num(a[1])) : std::atan(num(a[0])));
    });
    def("EXP", [](Interp&, A& a) { return Value::real(std::exp(num(argAt(a, 0)))); });
    def("LOG", [](Interp&, A& a) {
        double v = num(argAt(a, 0));
        if (v <= 0)
            throw Error("function undefined for arguments");
        return Value::real(std::log(v));
    });
    def("EXPT", [](Interp&, A& a) {
        needArgs(a, 2);
        double r = std::pow(num(a[0]), num(a[1]));
        if (a[0].kind == K::Int && a[1].kind == K::Int && a[1].i >= 0)
            return Value::integer(static_cast<long long>(std::llround(r)));
        return Value::real(r);
    });
    def("FIX", [](Interp&, A& a) {
        needArgs(a, 1);
        if (a[0].kind == K::Int)
            return a[0];
        double v = num(a[0]);
        if (!(std::fabs(v) < 9e18))
            throw Error("function undefined for arguments");
        return Value::integer(static_cast<long long>(v));
    });
    def("FLOAT", [](Interp&, A& a) { return Value::real(num(argAt(a, 0))); });
    def("GCD", [](Interp&, A& a) {
        needArgs(a, 2);
        long long x = std::llabs(a[0].i), y = std::llabs(a[1].i);
        while (y) {
            long long t = x % y;
            x = y;
            y = t;
        }
        return Value::integer(x);
    });
    def("ZEROP", [](Interp&, A& a) { return Value::boolean(num(argAt(a, 0)) == 0); });
    def("MINUSP", [](Interp&, A& a) { return Value::boolean(num(argAt(a, 0)) < 0); });
    def("NUMBERP", [](Interp&, A& a) { return Value::boolean(argAt(a, 0).isNumber()); });

    // ---- comparison / logic ----
    auto compare = [this](const char* name, bool (*ok)(int)) {
        defineSubr(name, [ok](Interp&, A& a) {
            needArgs(a, 1);
            for (std::size_t k = 0; k + 1 < a.size(); ++k) {
                int c;
                if (a[k].isNumber() && a[k + 1].isNumber()) {
                    double x = a[k].number(), y = a[k + 1].number();
                    c = x < y ? -1 : (x > y ? 1 : 0);
                } else if (a[k].kind == K::Str && a[k + 1].kind == K::Str) {
                    int d = a[k].s.compare(a[k + 1].s);
                    c = d < 0 ? -1 : (d > 0 ? 1 : 0);
                } else if (ok(0) && !ok(1) && !ok(-1)) {  // "=" on other types
                    c = equalValues(a[k], a[k + 1]) ? 0 : 1;
                } else {
                    bad();
                }
                if (!ok(c))
                    return Value::nil();
            }
            return Value::t();
        });
    };
    compare("=", [](int c) { return c == 0; });
    compare("<", [](int c) { return c < 0; });
    compare(">", [](int c) { return c > 0; });
    compare("<=", [](int c) { return c <= 0; });
    compare(">=", [](int c) { return c >= 0; });
    def("/=", [](Interp&, A& a) {
        needArgs(a, 2);
        return Value::boolean(!equalValues(a[0], a[1]));
    });
    def("EQ", [](Interp&, A& a) {
        needArgs(a, 2);
        if (a[0].kind == K::Pair && a[1].kind == K::Pair)
            return Value::boolean(a[0].pair == a[1].pair);
        return Value::boolean(equalValues(a[0], a[1]));
    });
    def("EQUAL", [](Interp&, A& a) {
        needArgs(a, 2);
        return Value::boolean(equalValues(a[0], a[1], a.size() > 2 ? num(a[2]) : 0));
    });
    def("NOT", [](Interp&, A& a) { return Value::boolean(argAt(a, 0).isNil()); });
    def("NULL", [](Interp&, A& a) { return Value::boolean(argAt(a, 0).isNil()); });
    def("ATOM", [](Interp&, A& a) { return Value::boolean(argAt(a, 0).kind != K::Pair); });
    def("LISTP", [](Interp&, A& a) {
        return Value::boolean(argAt(a, 0).kind == K::Pair || argAt(a, 0).isNil());
    });
    def("STRINGP", [](Interp&, A& a) { return Value::boolean(argAt(a, 0).kind == K::Str); });
    def("TYPE", [](Interp&, A& a) {
        switch (argAt(a, 0).kind) {
            case K::Nil:
                return Value::nil();
            case K::Int:
                return Value::sym("INT");
            case K::Real:
                return Value::sym("REAL");
            case K::Str:
                return Value::sym("STR");
            case K::Sym:
                return Value::sym("SYM");
            case K::Pair:
                return Value::sym("LIST");
            case K::Lambda:
                return Value::sym("USUBR");
            case K::Subr:
                return Value::sym("SUBR");
            case K::Ename:
                return Value::sym("ENAME");
            case K::PickSet:
                return Value::sym("PICKSET");
        }
        return Value::nil();
    });
    def("BOUNDP", [](Interp& in, A& a) {
        return Value::boolean(argAt(a, 0).kind == K::Sym && in.isBound(a[0].s));
    });

    // ---- lists ----
    def("CONS", [](Interp&, A& a) {
        needArgs(a, 2);
        return Value::cons(a[0], a[1]);
    });
    def("LIST", [](Interp&, A& a) { return Value::list(a); });
    for (int len = 1; len <= 4; ++len) {
        for (int bits = 0; bits < (1 << len); ++bits) {
            std::string name = "C";
            for (int k = 0; k < len; ++k)
                name += ((bits >> k) & 1) ? 'D' : 'A';
            name += 'R';
            std::string path =
                name.substr(1, static_cast<std::size_t>(len));  // applied right to left
            defineSubr(name, [path](Interp&, A& a) {
                needArgs(a, 1);
                Value v = a[0];
                for (auto it = path.rbegin(); it != path.rend(); ++it) {
                    if (v.isNil())
                        return Value::nil();
                    if (v.kind != K::Pair)
                        bad();
                    Value next = *it == 'A' ? v.pair->car : v.pair->cdr;
                    v = std::move(next);
                }
                return v;
            });
        }
    }
    def("LENGTH", [](Interp&, A& a) {
        const Value& v = argAt(a, 0);
        if (!v.isNil() && v.kind != K::Pair)
            bad();
        return Value::integer(static_cast<long long>(toVector(v).size()));
    });
    def("NTH", [](Interp&, A& a) {
        needArgs(a, 2);
        long long n = a[0].i;
        auto v = toVector(a[1]);
        if (n < 0 || n >= static_cast<long long>(v.size()))
            return Value::nil();
        return v[static_cast<std::size_t>(n)];
    });
    def("LAST", [](Interp&, A& a) {
        auto v = toVector(argAt(a, 0));
        return v.empty() ? Value::nil() : v.back();
    });
    def("REVERSE", [](Interp&, A& a) {
        auto v = toVector(argAt(a, 0));
        std::reverse(v.begin(), v.end());
        return Value::list(v);
    });
    def("APPEND", [](Interp&, A& a) {
        std::vector<Value> all;
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (a[k].isNil())
                continue;
            if (a[k].kind != K::Pair)
                bad();
            auto v = toVector(a[k]);
            all.insert(all.end(), v.begin(), v.end());
        }
        return Value::list(all);
    });
    def("MEMBER", [](Interp&, A& a) {
        needArgs(a, 2);
        const Value* p = &a[1];
        while (p->kind == K::Pair) {
            if (equalValues(a[0], p->pair->car))
                return *p;
            p = &p->pair->cdr;
        }
        return Value::nil();
    });
    def("ASSOC", [](Interp&, A& a) {
        needArgs(a, 2);
        for (const auto& e : toVector(a[1]))
            if (e.kind == K::Pair && equalValues(a[0], e.pair->car))
                return e;
        return Value::nil();
    });
    def("SUBST", [](Interp&, A& a) {
        needArgs(a, 3);
        auto v = toVector(a[2]);
        for (auto& e : v)
            if (equalValues(e, a[1]))
                e = a[0];
        return Value::list(v);
    });
    def("REMOVE", [](Interp&, A& a) {
        needArgs(a, 2);
        std::vector<Value> out;
        for (auto& e : toVector(a[1]))
            if (!equalValues(e, a[0]))
                out.push_back(e);
        return Value::list(out);
    });
    def("VL-REMOVE", [](Interp&, A& a) {
        needArgs(a, 2);
        std::vector<Value> out;
        for (auto& e : toVector(a[1]))
            if (!equalValues(e, a[0]))
                out.push_back(e);
        return Value::list(out);
    });
    def("VL-POSITION", [](Interp&, A& a) {
        needArgs(a, 2);
        long long k = 0;
        for (auto& e : toVector(a[1])) {
            if (equalValues(e, a[0]))
                return Value::integer(k);
            ++k;
        }
        return Value::nil();
    });
    def("VL-SORT", [](Interp& in, A& a) {
        needArgs(a, 2);
        auto v = toVector(a[0]);
        std::stable_sort(v.begin(), v.end(), [&](const Value& x, const Value& y) {
            return !in.call(a[1], {x, y}).isNil();
        });
        return Value::list(v);
    });
    def("APPLY", [](Interp& in, A& a) {
        needArgs(a, 2);
        return in.call(a[0], toVector(a[1]));
    });
    def("MAPCAR", [](Interp& in, A& a) {
        needArgs(a, 2);
        std::vector<std::vector<Value>> lists;
        std::size_t n = SIZE_MAX;
        for (std::size_t k = 1; k < a.size(); ++k) {
            lists.push_back(toVector(a[k]));
            n = std::min(n, lists.back().size());
        }
        std::vector<Value> out;
        for (std::size_t k = 0; k < n; ++k) {
            Args call;
            for (auto& l : lists)
                call.push_back(l[k]);
            out.push_back(in.call(a[0], std::move(call)));
        }
        return Value::list(out);
    });
    def("VL-CATCH-ALL-APPLY", [](Interp& in, A& a) {
        needArgs(a, 2);
        try {
            return in.call(a[0], toVector(a[1]));
        } catch (const Error& e) {
            return Value::cons(Value::sym("*ERROR-OBJECT*"), Value::str(e.what()));
        }
    });
    def("VL-CATCH-ALL-ERROR-P", [](Interp&, A& a) {
        const Value& v = argAt(a, 0);
        return Value::boolean(v.kind == K::Pair && v.pair->car.kind == K::Sym &&
                              v.pair->car.s == "*ERROR-OBJECT*");
    });
    def("VL-CATCH-ALL-ERROR-MESSAGE", [](Interp&, A& a) {
        const Value& v = argAt(a, 0);
        if (v.kind != K::Pair)
            bad();
        return v.pair->cdr;
    });

    // ---- strings ----
    def("STRCAT", [](Interp&, A& a) {
        std::string s;
        for (auto& v : a)
            s += text(v);
        return Value::str(std::move(s));
    });
    def("STRLEN", [](Interp&, A& a) {
        long long n = 0;
        for (auto& v : a)
            n += static_cast<long long>(text(v).size());
        return Value::integer(n);
    });
    def("SUBSTR", [](Interp&, A& a) {
        needArgs(a, 2);
        const auto& s = text(a[0]);
        long long start = a[1].i;
        if (start < 1)
            throw Error("invalid argument");
        long long len = a.size() > 2 ? a[2].i : static_cast<long long>(s.size());
        if (len < 0)
            throw Error("invalid argument");
        if (start > static_cast<long long>(s.size()))
            return Value::str("");
        return Value::str(
            s.substr(static_cast<std::size_t>(start - 1), static_cast<std::size_t>(len)));
    });
    def("STRCASE", [](Interp&, A& a) {
        needArgs(a, 1);
        std::string s = text(a[0]);
        bool lower = a.size() > 1 && !a[1].isNil();
        for (auto& c : s)
            c = static_cast<char>(lower ? std::tolower(static_cast<unsigned char>(c))
                                        : std::toupper(static_cast<unsigned char>(c)));
        return Value::str(std::move(s));
    });
    def("ITOA", [](Interp&, A& a) {
        needArgs(a, 1);
        if (a[0].kind != K::Int)
            bad();
        return Value::str(std::to_string(a[0].i));
    });
    def("ATOI", [](Interp&, A& a) {
        return Value::integer(std::strtoll(text(argAt(a, 0)).c_str(), nullptr, 10));
    });
    def("ATOF",
        [](Interp&, A& a) { return Value::real(std::strtod(text(argAt(a, 0)).c_str(), nullptr)); });
    def("RTOS", [](Interp&, A& a) {
        needArgs(a, 1);
        int mode = a.size() > 1 ? static_cast<int>(a[1].i) : 2;
        int prec = a.size() > 2 ? static_cast<int>(a[2].i) : 4;
        return Value::str(rtosText(num(a[0]), mode, prec));
    });
    def("CHR",
        [](Interp&, A& a) { return Value::str(std::string(1, static_cast<char>(argAt(a, 0).i))); });
    def("ASCII", [](Interp&, A& a) {
        const auto& s = text(argAt(a, 0));
        return Value::integer(s.empty() ? 0 : static_cast<unsigned char>(s[0]));
    });
    def("WCMATCH", [](Interp&, A& a) {
        needArgs(a, 2);
        const std::string& s = text(a[0]);
        const std::string& pats = text(a[1]);
        std::size_t from = 0;
        for (;;) {
            auto comma = pats.find(',', from);
            if (wildcardMatch(pats.substr(from, comma == std::string::npos ? comma : comma - from), s))
                return Value::t();
            if (comma == std::string::npos)
                return Value::nil();
            from = comma + 1;
        }
    });
    def("VL-STRING-SEARCH", [](Interp&, A& a) {
        needArgs(a, 2);
        auto pos = text(a[1]).find(text(a[0]), a.size() > 2 ? static_cast<std::size_t>(a[2].i) : 0);
        return pos == std::string::npos ? Value::nil()
                                        : Value::integer(static_cast<long long>(pos));
    });
    def("VL-PRINC-TO-STRING",
        [](Interp&, A& a) { return Value::str(toString(argAt(a, 0), false)); });
    def("VL-PRIN1-TO-STRING",
        [](Interp&, A& a) { return Value::str(toString(argAt(a, 0), true)); });
    def("READ", [](Interp&, A& a) {
        auto forms = readAll(text(argAt(a, 0)));
        return forms.empty() ? Value::nil() : forms[0];
    });

    // ---- geometry helpers ----
    def("DISTANCE", [](Interp&, A& a) {
        needArgs(a, 2);
        auto p = coords(a[0]);
        auto q = coords(a[1]);
        double dz = (p.size() > 2 ? p[2] : 0) - (q.size() > 2 ? q[2] : 0);
        return Value::real(std::hypot(std::hypot(p[0] - q[0], p[1] - q[1]), dz));
    });
    def("ANGLE", [](Interp&, A& a) {
        needArgs(a, 2);
        auto p = coords(a[0]);
        auto q = coords(a[1]);
        double ang = std::atan2(q[1] - p[1], q[0] - p[0]);
        if (ang < 0)
            ang += 2 * std::numbers::pi;
        return Value::real(ang);
    });
    def("POLAR", [](Interp&, A& a) {
        needArgs(a, 3);
        auto p = coords(a[0]);
        double ang = num(a[1]), d = num(a[2]);
        return pointValue(p[0] + d * std::cos(ang), p[1] + d * std::sin(ang),
                          p.size() > 2 ? p[2] : 0);
    });

    // ---- output ----
    def("PRINC", [](Interp& in, A& a) {
        if (!a.empty() && in.out)
            in.out(toString(a[0], false));
        return argAt(a, 0);
    });
    def("PRIN1", [](Interp& in, A& a) {
        if (!a.empty() && in.out)
            in.out(toString(a[0], true));
        return argAt(a, 0);
    });
    def("PRINT", [](Interp& in, A& a) {
        if (in.out)
            in.out("\n" + toString(argAt(a, 0), true) + " ");
        return argAt(a, 0);
    });
    def("PROMPT", [](Interp& in, A& a) {
        if (in.out)
            in.out(text(argAt(a, 0)));
        return Value::nil();
    });
    def("TERPRI", [](Interp& in, A&) {
        if (in.out)
            in.out("\n");
        return Value::nil();
    });
    def("WRITE-LINE", [](Interp& in, A& a) {
        if (in.out)
            in.out(text(argAt(a, 0)) + "\n");
        return argAt(a, 0);
    });

    // ---- program ----
    def("EVAL", [](Interp& in, A& a) { return in.eval(argAt(a, 0)); });
    def("EXIT", [](Interp&, A&) -> Value { throw Abort(); });
    def("QUIT", [](Interp&, A&) -> Value { throw Abort(); });
    def("LOAD", [](Interp& in, A& a) {
        const std::string& name = text(argAt(a, 0));
        std::string src;
        if (!in.loader || !in.loader(name, src)) {
            if (a.size() > 1)
                return a[1];
            throw Error("LOAD failed: \"" + name + "\"");
        }
        return in.evalText(src);
    });
}

void Interp::defineSubr(std::string name, Subr fn) {
    subrs_[upper(std::move(name))] = std::move(fn);
}
void Interp::set(std::string_view name, Value v) {
    globals_[upper(std::string(name))] = std::move(v);
}
Value Interp::get(std::string_view name) const {
    auto it = globals_.find(upper(std::string(name)));
    return it == globals_.end() ? Value{} : it->second;
}
bool Interp::isBound(std::string_view name) const {
    std::string n = upper(std::string(name));
    auto it = globals_.find(n);
    return (it != globals_.end() && !it->second.isNil()) || subrs_.contains(n);
}

Value Interp::evalText(std::string_view src) {
    auto forms = readAll(src);
    Value last;
    steps_ = 0;
    for (auto& f : forms) {
        try {
            last = eval(f);
        } catch (const Error& e) {
            auto it = globals_.find("*ERROR*");
            if (it != globals_.end() && it->second.kind == K::Lambda) {
                try {
                    call(it->second, {Value::str(e.what())});
                } catch (const Error&) {
                }
            }
            throw;
        }
    }
    return last;
}

Value Interp::call(const Value& fn, Args args) {
    return apply(fn, args);
}

Value Interp::apply(const Value& fn, Args& args) {
    if (fn.kind == K::Lambda)
        return callLambda(*fn.fn, args);
    if (fn.kind == K::Subr || fn.kind == K::Sym) {
        auto s = subrs_.find(fn.s);
        if (s != subrs_.end())
            return s->second(*this, args);
        if (fn.kind == K::Sym) {
            auto g = globals_.find(fn.s);
            if (g != globals_.end() && g->second.kind == K::Lambda)
                return callLambda(*g->second.fn, args);
        }
        throw Error("no function definition: " + fn.s);
    }
    throw Error("bad function");
}

Value Interp::callLambda(const Lambda& fn, Args& args) {
    if (depth_ >= maxDepth)
        throw Error("stack overflow");
    struct Frame {
        Interp& in;
        std::vector<std::pair<std::string, std::optional<Value>>> saved;
        ~Frame() {
            --in.depth_;
            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                if (it->second)
                    in.globals_[it->first] = std::move(*it->second);
                else
                    in.globals_.erase(it->first);
            }
        }
    } frame{*this, {}};
    ++depth_;
    auto bind = [&](const std::string& name, Value v) {
        auto it = globals_.find(name);
        frame.saved.emplace_back(
            name, it == globals_.end() ? std::nullopt : std::optional<Value>(it->second));
        globals_[name] = std::move(v);
    };
    for (std::size_t k = 0; k < fn.params.size(); ++k)
        bind(fn.params[k], k < args.size() ? args[k] : Value{});
    for (const auto& l : fn.locals)
        bind(l, Value{});
    Value last;
    for (const auto& form : fn.body)
        last = eval(form);
    return last;
}

Value Interp::eval(const Value& form) {
    if (++steps_ % 4096 == 0 && poll)
        poll();
    if (stepBudget && steps_ > stepBudget)
        throw Error("program ran too long");
    switch (form.kind) {
        case K::Sym: {
            auto it = globals_.find(form.s);
            if (it != globals_.end())
                return it->second;
            if (form.s == "T")
                return form;
            if (subrs_.contains(form.s)) {
                Value v;
                v.kind = K::Subr;
                v.s = form.s;
                return v;
            }
            return Value::nil();
        }
        case K::Pair:
            return evalList(form);
        default:
            return form;  // numbers, strings, nil evaluate to themselves
    }
}

Value Interp::evalList(const Value& form) {
    const Value& head = form.pair->car;
    const Value& rest = form.pair->cdr;
    if (head.kind == K::Sym) {
        const auto& sf = SpecialForms::table();
        if (auto it = sf.find(head.s); it != sf.end())
            return it->second(*this, rest);
    }
    Args args;
    for (const Value* p = &rest; p->kind == K::Pair; p = &p->pair->cdr)
        args.push_back(eval(p->pair->car));
    if (head.kind == K::Sym) {
        auto s = subrs_.find(head.s);
        if (s != subrs_.end())
            return s->second(*this, args);
        auto g = globals_.find(head.s);
        if (g != globals_.end() && g->second.kind == K::Lambda)
            return callLambda(*g->second.fn, args);
        throw Error("no function definition: " + head.s);
    }
    if (head.kind == K::Pair) {
        Value f = eval(head);
        return apply(f, args);
    }
    throw Error("bad function");
}

// ---- special forms
// -------------------------------------------------------------------------------

namespace {

Value progn(Interp& in, const Value& body) {
    Value last;
    for (const Value* p = &body; p->kind == K::Pair; p = &p->pair->cdr)
        last = in.eval(p->pair->car);
    return last;
}

Value makeLambda(const Value& params, const Value& body) {
    auto fn = std::make_shared<Lambda>();
    bool locals = false;
    for (const Value* p = &params; p->kind == K::Pair; p = &p->pair->cdr) {
        const Value& e = p->pair->car;
        if (e.kind != K::Sym)
            throw Error("bad argument list");
        if (e.s == "/")
            locals = true;
        else
            (locals ? fn->locals : fn->params).push_back(e.s);
    }
    fn->body = toVector(body);
    Value v;
    v.kind = K::Lambda;
    v.fn = std::move(fn);
    return v;
}

const Value& drop(const Value& list, int n) {
    const Value* p = &list;
    while (n-- > 0 && p->kind == K::Pair)
        p = &p->pair->cdr;
    return *p;
}

const Value& nth(const Value& list, int n) {
    static const Value nilValue;
    const Value* p = &list;
    while (n-- > 0 && p->kind == K::Pair)
        p = &p->pair->cdr;
    return p->kind == K::Pair ? p->pair->car : nilValue;
}

}  // namespace

const std::unordered_map<std::string, SpecialForms::Fn>& SpecialForms::table() {
    static const std::unordered_map<std::string, Fn> t = {
        {"QUOTE", [](Interp&, const Value& r) { return nth(r, 0); }},
        {"FUNCTION",
         [](Interp& in, const Value& r) {
             const Value& f = nth(r, 0);
             bool isLambda =
                 f.kind == K::Pair && f.pair->car.kind == K::Sym && f.pair->car.s == "LAMBDA";
             return isLambda ? in.eval(f) : f;
         }},
        {"PROGN", [](Interp& in, const Value& r) { return progn(in, r); }},
        {"SETQ",
         [](Interp& in, const Value& r) {
             Value last;
             for (const Value* p = &r; p->kind == K::Pair; p = &p->pair->cdr) {
                 const Value& name = p->pair->car;
                 if (name.kind != K::Sym)
                     throw Error("bad argument type");
                 p = &p->pair->cdr;
                 if (p->kind != K::Pair) {
                     in.set(name.s, Value{});
                     break;
                 }
                 last = in.eval(p->pair->car);
                 in.set(name.s, last);
             }
             return last;
         }},
        {"IF",
         [](Interp& in, const Value& r) {
             if (!in.eval(nth(r, 0)).isNil())
                 return in.eval(nth(r, 1));
             return in.eval(nth(r, 2));
         }},
        {"COND",
         [](Interp& in, const Value& r) {
             for (const Value* p = &r; p->kind == K::Pair; p = &p->pair->cdr) {
                 const Value& clause = p->pair->car;
                 Value test = in.eval(nth(clause, 0));
                 if (test.isNil())
                     continue;
                 if (drop(clause, 1).isNil())
                     return test;
                 return progn(in, drop(clause, 1));
             }
             return Value{};
         }},
        {"AND",
         [](Interp& in, const Value& r) {
             Value last = Value::t();
             for (const Value* p = &r; p->kind == K::Pair; p = &p->pair->cdr) {
                 last = in.eval(p->pair->car);
                 if (last.isNil())
                     return Value{};
             }
             return last;
         }},
        {"OR",
         [](Interp& in, const Value& r) {
             for (const Value* p = &r; p->kind == K::Pair; p = &p->pair->cdr) {
                 Value v = in.eval(p->pair->car);
                 if (!v.isNil())
                     return v;
             }
             return Value{};
         }},
        {"WHILE",
         [](Interp& in, const Value& r) {
             while (!in.eval(nth(r, 0)).isNil())
                 progn(in, drop(r, 1));
             return Value{};
         }},
        {"REPEAT",
         [](Interp& in, const Value& r) {
             Value n = in.eval(nth(r, 0));
             if (n.kind != K::Int)
                 throw Error("bad argument type");
             Value last;
             for (long long k = 0; k < n.i; ++k)
                 last = progn(in, drop(r, 1));
             return last;
         }},
        {"FOREACH",
         [](Interp& in, const Value& r) {
             const Value& var = nth(r, 0);
             if (var.kind != K::Sym)
                 throw Error("bad argument type");
             Value saved = in.get(var.s);
             Value last;
             for (const auto& item : toVector(in.eval(nth(r, 1)))) {
                 in.set(var.s, item);
                 last = progn(in, drop(r, 2));
             }
             in.set(var.s, saved);
             return last;
         }},
        {"LAMBDA", [](Interp&, const Value& r) { return makeLambda(nth(r, 0), drop(r, 1)); }},
        {"DEFUN",
         [](Interp& in, const Value& r) {
             const Value& name = nth(r, 0);
             if (name.kind != K::Sym)
                 throw Error("bad argument type");
             in.set(name.s, makeLambda(nth(r, 1), drop(r, 2)));
             if (name.s.rfind("C:", 0) == 0 && in.onCommandDefined)
                 in.onCommandDefined(name.s.substr(2));
             return name;
         }},
    };
    return t;
}

}  // namespace tbcad::lisp
