#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "tbcad/constraints/constraints.h"
#include "tbcad/expr/expr.h"

namespace tbcad::constraints {

namespace {

Error bad(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

bool validName(const std::string& n) {
    if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0])))
        return false;
    return std::all_of(n.begin(), n.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

// Evaluates parameters on demand, each once, and spots circular definitions.
class Evaluator {
public:
    explicit Evaluator(const ParameterSet& ps) : ps_(ps) {}

    Result<double> run(const std::string& source) {
        auto parsed = expr::Expression::parse(source);
        if (!parsed)
            return parsed.error();
        auto v = parsed.value().eval(env());
        if (!error_.empty())
            return bad(error_);
        if (!v)
            return v.error();
        if (v.value().isText)
            return bad("the formula \"" + source + "\" gives text, not a number");
        if (!std::isfinite(v.value().number))
            return bad("the formula \"" + source + "\" is not a finite number");
        return v.value().number;
    }

    const std::map<std::string, double>& memo() const { return memo_; }

private:
    expr::Env env() {
        return [this](std::string_view name) -> std::optional<expr::Value> {
            const std::string n(name);
            if (auto it = memo_.find(n); it != memo_.end())
                return expr::Value::ofNumber(it->second);
            const Parameter* p = ps_.find(n);
            if (!p)
                return std::nullopt;
            if (!active_.insert(n).second) {
                if (error_.empty())
                    error_ = "the parameter " + n + " depends on itself";
                return std::nullopt;
            }
            auto v = run(p->expr);
            active_.erase(n);
            if (!v) {
                if (error_.empty())
                    error_ = "parameter " + n + ": " + v.error().message;
                return std::nullopt;
            }
            memo_[n] = v.value();
            return expr::Value::ofNumber(v.value());
        };
    }

    const ParameterSet& ps_;
    std::map<std::string, double> memo_{};
    std::set<std::string> active_{};
    std::string error_{};
};

}  // namespace

const Parameter* ParameterSet::find(const std::string& name) const {
    for (const Parameter& p : items_)
        if (p.name == name)
            return &p;
    return nullptr;
}

Result<void> ParameterSet::set(const std::string& name, const std::string& expr) {
    if (!validName(name))
        return bad("\"" + name +
                   "\" is not a parameter name (letters, digits and _, not starting "
                   "with a digit)");
    auto parsed = expr::Expression::parse(expr);
    if (!parsed)
        return parsed.error();
    ParameterSet trial = *this;
    if (Parameter* p = const_cast<Parameter*>(trial.find(name)))
        p->expr = expr;
    else
        trial.items_.push_back({name, expr});
    // Constants and functions cannot be redefined: a name like "pi" never reaches the parameter
    // lookup, so such a parameter would silently do nothing.
    if (auto r = expr::Expression::parse(name); !r || r.value().names().empty())
        return bad("\"" + name + "\" is already a constant or function name");
    auto vals = trial.values();
    if (!vals)
        return vals.error();
    *this = std::move(trial);
    return {};
}

bool ParameterSet::remove(const std::string& name) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->name == name) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

Result<std::map<std::string, double>> ParameterSet::values() const {
    Evaluator ev(*this);
    for (const Parameter& p : items_) {
        if (ev.memo().count(p.name))
            continue;
        // Evaluating the name itself pulls in everything it needs.
        auto v = ev.run(p.name);
        if (!v)
            return v.error();
    }
    return ev.memo();
}

Result<double> ParameterSet::evaluate(const std::string& source) const {
    Evaluator ev(*this);
    return ev.run(source);
}

std::vector<std::string> ParameterSet::unknownNames(const std::string& source) const {
    std::vector<std::string> out;
    if (auto parsed = expr::Expression::parse(source))
        for (const std::string& n : parsed.value().names())
            if (!find(n))
                out.push_back(n);
    return out;
}

std::string ParameterSet::toText() const {
    std::string out;
    for (const Parameter& p : items_)
        out += p.name + "\t" + p.expr + "\n";
    return out;
}

ParameterSet ParameterSet::fromText(const std::string& text) {
    ParameterSet set;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        std::string expr = line.substr(tab + 1);
        while (!expr.empty() && (expr.back() == '\r' || expr.back() == '\n'))
            expr.pop_back();
        const std::string name = line.substr(0, tab);
        if (validName(name) && !set.find(name))
            set.items_.push_back({name, expr});
    }
    return set;
}

ParameterSet loadParameters(const Document& doc) {
    auto it = doc.header().custom.find(kParametersKey);
    return it == doc.header().custom.end() ? ParameterSet{} : ParameterSet::fromText(it->second);
}

Result<void> storeParameters(Transaction& tx, const ParameterSet& set) {
    const std::string text = set.toText();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kParametersKey);
        else
            h.custom[kParametersKey] = text;
    });
}

Result<SolveResult> applyParameters(Transaction& tx, ConstraintSet& set,
                                    const ParameterSet& params) {
    ConstraintSet next = set;
    for (const Constraint& c : set.all()) {
        if (c.expr.empty())
            continue;
        auto v = params.evaluate(c.expr);
        if (!v)
            return bad("constraint #" + std::to_string(c.id) + ": " + v.error().message);
        double value = v.value();
        if (c.kind == Kind::Angle)
            value *= kPi / 180.0;
        if ((c.kind == Kind::Radius && !(value > 0)) || (c.kind == Kind::Distance && value < 0))
            return bad("constraint #" + std::to_string(c.id) + " would get the value " +
                       std::to_string(v.value()));
        next.find(c.id)->value = value;
    }
    auto res = solve(tx, next);
    if (!res)
        return res;
    if (res.value().converged) {
        if (auto stored = store(tx, next); !stored)
            return stored.error();
        set = std::move(next);
    }
    return res;
}

}  // namespace tbcad::constraints
