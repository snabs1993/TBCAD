#include "tbcad/blocks/assembly.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "tbcad/blocks/array.h"
#include "tbcad/blocks/dynamic.h"

namespace tbcad::blocks {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return r;
}

// Parsed formulas, shared between threads. Formulas are short and few, so one lock is enough.
Result<expr::Expression> parsed(const std::string& formula) {
    static std::mutex mutex;
    static std::unordered_map<std::string, expr::Expression> cache;
    {
        std::lock_guard lock(mutex);
        auto it = cache.find(formula);
        if (it != cache.end())
            return it->second;
    }
    auto e = expr::Expression::parse(formula);
    if (e) {
        std::lock_guard lock(mutex);
        if (cache.size() > 4096)
            cache.clear();
        cache.emplace(formula, e.value());
    }
    return e;
}

double clampNumber(const AsmParam& p, double v) {
    if (!std::isfinite(v))
        v = p.defaultNumber;
    if (p.max > p.min)
        v = std::clamp(v, p.min, p.max);
    if (p.kind == AsmParam::Kind::Integer)
        v = std::round(v);
    return v;
}

AsmValue defaultValue(const AsmParam& p) {
    AsmValue v;
    v.name = p.name;
    if (p.kind == AsmParam::Kind::Text) {
        v.isText = true;
        v.text = p.defaultText;
    } else if (p.kind == AsmParam::Kind::Choice) {
        v.isText = true;
        v.text = !p.defaultText.empty() ? p.defaultText
                                        : (p.choices.empty() ? std::string{} : p.choices.front());
    } else {
        v.number = clampNumber(p, p.defaultNumber);
    }
    return v;
}

AsmValue normalized(const AsmParam& p, const AsmValue& in) {
    AsmValue out = defaultValue(p);
    if (p.kind == AsmParam::Kind::Text) {
        out.text = in.isText ? in.text : expr::Value::ofNumber(in.number).toString();
    } else if (p.kind == AsmParam::Kind::Choice) {
        std::string t = in.isText ? in.text : expr::Value::ofNumber(in.number).toString();
        if (std::find(p.choices.begin(), p.choices.end(), t) != p.choices.end())
            out.text = t;
    } else {
        double v = in.isText ? expr::Value::ofText(in.text).toNumber() : in.number;
        out.number = clampNumber(p, v);
    }
    return out;
}

expr::Value toExpr(const AsmValue& v) {
    return v.isText ? expr::Value::ofText(v.text) : expr::Value::ofNumber(v.number);
}

}  // namespace

int asmFind(const AssemblyDef& def, std::string_view name) {
    const std::string want = lower(name);
    for (std::size_t i = 0; i < def.params.size(); ++i)
        if (lower(def.params[i].name) == want)
            return static_cast<int>(i);
    return -1;
}

std::vector<AsmValue> asmEffectiveValues(const AssemblyDef& def, const InsertData& insert) {
    std::vector<AsmValue> out;
    out.reserve(def.params.size());
    for (const AsmParam& p : def.params) {
        const AsmValue* own = nullptr;
        for (const AsmValue& v : insert.asmValues)
            if (lower(v.name) == lower(p.name)) {
                own = &v;
                break;
            }
        out.push_back(own ? normalized(p, *own) : defaultValue(p));
    }
    return out;
}

namespace {

// Sets dynamic parameter `idx` of `ins` from a formula value.
void setDynValue(const BlockDef& def, InsertData& ins, int idx, const expr::Value& v) {
    const DynParam& p = def.dynamic->params[static_cast<std::size_t>(idx)];
    std::vector<double> values = dynValues(def, ins);
    double x = 0;
    if (p.kind == DynParam::Kind::Visibility && v.isText) {
        x = 0;
        for (std::size_t i = 0; i < p.states.size(); ++i)
            if (p.states[i] == v.text)
                x = static_cast<double>(i);
    } else {
        x = v.toNumber();
    }
    values[static_cast<std::size_t>(idx)] = x;
    ins.dynValues = std::move(values);
}

void setAttribute(InsertData& ins, const std::string& tag, const std::string& text) {
    for (AttribValue& a : ins.attribs)
        if (a.tag == tag) {
            a.value = text;
            return;
        }
    ins.attribs.push_back(AttribValue{tag, text});
}

}  // namespace

AsmResolved resolveAssembly(const Document& doc, const BlockDef& def, const InsertData& insert) {
    (void)doc;
    AsmResolved out;
    out.insert = insert;
    if (!def.assembly)
        return out;
    const std::vector<AsmValue> values = asmEffectiveValues(*def.assembly, insert);
    out.insert.asmValues = values;
    const expr::Env env = [&](std::string_view name) -> std::optional<expr::Value> {
        const int i = asmFind(*def.assembly, name);
        if (i < 0)
            return std::nullopt;
        return toExpr(values[static_cast<std::size_t>(i)]);
    };
    for (const AsmBinding& b : def.assembly->bindings) {
        auto e = parsed(b.formula);
        if (!e) {
            out.errors.push_back(b.target + ": " + e.error().message);
            continue;
        }
        auto v = e.value().eval(env);
        if (!v) {
            out.errors.push_back(b.target + ": " + v.error().message);
            continue;
        }
        switch (b.kind) {
            case AsmBinding::Kind::DynParam: {
                const int idx = def.dynamic ? dynFind(*def.dynamic, b.target) : -1;
                if (idx < 0)
                    out.errors.push_back("no dynamic parameter \"" + b.target + "\"");
                else
                    setDynValue(def, out.insert, idx, v.value());
                break;
            }
            case AsmBinding::Kind::Attribute:
                setAttribute(out.insert, b.target, v.value().toString());
                break;
            case AsmBinding::Kind::Nested:
            case AsmBinding::Kind::ArrayField:
                out.nested.push_back({b.kind, b.entity, b.target, std::move(v).value()});
                break;
        }
    }
    return out;
}

void applyNested(const Document& doc, std::vector<Entity>& items,
                 const std::vector<NestedSet>& nested) {
    for (const NestedSet& n : nested) {
        for (Entity& e : items) {
            if (!(e.common.id == n.entity))
                continue;
            auto* ins = entityAs<InsertData>(e);
            if (!ins)
                break;
            if (n.kind == AsmBinding::Kind::ArrayField) {
                if (!ins->array)
                    break;
                ArrayInfo& a = *ins->array;
                const double x = n.value.toNumber();
                auto count = [&](int& field) {
                    field = static_cast<int>(
                        std::clamp(std::round(x), 1.0, static_cast<double>(kMaxArrayItems)));
                };
                if (n.target == "rows")
                    count(a.rows);
                else if (n.target == "columns")
                    count(a.columns);
                else if (n.target == "count")
                    count(a.count);
                else if (n.target == "rowSpacing")
                    a.rowSpacing = x;
                else if (n.target == "columnSpacing")
                    a.columnSpacing = x;
                else if (n.target == "pathSpacing")
                    a.pathSpacing = x;
                break;
            }
            const BlockDef* inner = doc.block(ins->block);
            if (!inner)
                break;
            if (inner->assembly) {
                const int pi = asmFind(*inner->assembly, n.target);
                if (pi >= 0) {
                    AsmValue v;
                    v.name = inner->assembly->params[static_cast<std::size_t>(pi)].name;
                    v.isText = n.value.isText;
                    v.number = n.value.number;
                    v.text = n.value.text;
                    bool done = false;
                    for (AsmValue& own : ins->asmValues)
                        if (lower(own.name) == lower(v.name)) {
                            own = v;
                            done = true;
                        }
                    if (!done)
                        ins->asmValues.push_back(std::move(v));
                    break;
                }
            }
            if (inner->dynamic) {
                const int di = dynFind(*inner->dynamic, n.target);
                if (di >= 0)
                    setDynValue(*inner, *ins, di, n.value);
            }
            break;
        }
    }
}

Result<Entity> asmSet(const Document& doc, const Entity& insert, std::string_view name,
                      const expr::Value& value) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || !def->assembly)
        return makeError(ErrorCode::InvalidArgument, "This block is not a parametric assembly");
    const int pi = asmFind(*def->assembly, name);
    if (pi < 0)
        return makeError(ErrorCode::NotFound,
                         "There is no parameter called \"" + std::string(name) + "\"");
    const AsmParam& p = def->assembly->params[static_cast<std::size_t>(pi)];
    AsmValue v;
    v.name = p.name;
    v.isText = value.isText;
    v.number = value.number;
    v.text = value.text;
    if (p.kind == AsmParam::Kind::Choice) {
        const std::string t = value.toString();
        if (std::find(p.choices.begin(), p.choices.end(), t) == p.choices.end()) {
            std::string list;
            for (const std::string& c : p.choices)
                list += (list.empty() ? "" : ", ") + c;
            return makeError(ErrorCode::OutOfRange,
                             "\"" + t + "\" is not one of the choices (" + list + ")");
        }
        v.isText = true;
        v.text = t;
    } else if (p.kind == AsmParam::Kind::Text) {
        v.isText = true;
        v.text = value.toString();
    } else {
        if (value.isText && value.toNumber() == 0 && value.text != "0")
            return makeError(ErrorCode::InvalidArgument, "\"" + value.text + "\" is not a number");
        v.isText = false;
        v.number = value.toNumber();
        if (p.max > p.min && (v.number < p.min || v.number > p.max))
            return makeError(ErrorCode::OutOfRange, p.name + " must be between " +
                                                        expr::Value::ofNumber(p.min).toString() +
                                                        " and " +
                                                        expr::Value::ofNumber(p.max).toString());
        if (p.kind == AsmParam::Kind::Integer && v.number != std::round(v.number))
            return makeError(ErrorCode::InvalidArgument, p.name + " must be a whole number");
    }
    Entity out = insert;
    auto& d = *entityAs<InsertData>(out);
    d.asmValues = asmEffectiveValues(*def->assembly, *ins);
    d.asmValues[static_cast<std::size_t>(pi)] = std::move(v);
    return out;
}

Result<Entity> asmApplyPreset(const Document& doc, const Entity& insert, std::string_view preset) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || !def->assembly)
        return makeError(ErrorCode::InvalidArgument, "This block is not a parametric assembly");
    const AsmPreset* found = nullptr;
    for (const AsmPreset& p : def->assembly->presets)
        if (lower(p.name) == lower(preset))
            found = &p;
    if (!found)
        return makeError(ErrorCode::NotFound,
                         "There is no preset called \"" + std::string(preset) + "\"");
    Entity out = insert;
    for (const AsmValue& v : found->values) {
        auto r = asmSet(doc, out, v.name,
                        v.isText ? expr::Value::ofText(v.text) : expr::Value::ofNumber(v.number));
        if (!r)
            return r;
        out = std::move(r).value();
    }
    return out;
}

std::vector<std::string> asmValidate(const Document& doc, const BlockDef& def) {
    std::vector<std::string> problems;
    if (!def.assembly)
        return problems;
    std::vector<std::string> seen;
    for (const AsmParam& p : def.assembly->params) {
        const std::string key = lower(p.name);
        if (p.name.empty())
            problems.push_back("A parameter has no name");
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            problems.push_back("Two parameters are called \"" + p.name + "\"");
        seen.push_back(key);
        if (p.kind == AsmParam::Kind::Choice && p.choices.empty())
            problems.push_back(p.name + " has no choices");
    }
    for (const AsmBinding& b : def.assembly->bindings) {
        auto e = parsed(b.formula);
        if (!e) {
            problems.push_back(b.target + ": " + e.error().message);
            continue;
        }
        for (const std::string& n : e.value().names())
            if (asmFind(*def.assembly, n) < 0)
                problems.push_back(b.target + ": the formula uses \"" + n +
                                   "\", which is not a parameter");
        switch (b.kind) {
            case AsmBinding::Kind::DynParam:
                if (!def.dynamic || dynFind(*def.dynamic, b.target) < 0)
                    problems.push_back("There is no dynamic parameter \"" + b.target + "\"");
                break;
            case AsmBinding::Kind::Attribute: {
                bool found = false;
                for (EntityId id : def.entities)
                    if (const Entity* en = doc.find(id))
                        if (const auto* t = entityAs<TextData>(*en);
                            t && t->attdef && t->attdef->tag == b.target)
                            found = true;
                if (!found)
                    problems.push_back("There is no attribute \"" + b.target + "\"");
                break;
            }
            case AsmBinding::Kind::Nested:
            case AsmBinding::Kind::ArrayField: {
                const Entity* en = doc.find(b.entity);
                const auto* ni = en ? entityAs<InsertData>(*en) : nullptr;
                if (!ni || std::find(def.entities.begin(), def.entities.end(), b.entity) ==
                               def.entities.end()) {
                    problems.push_back("The nested insert for \"" + b.target +
                                       "\" is not in the block");
                    break;
                }
                if (b.kind == AsmBinding::Kind::ArrayField) {
                    if (!ni->array)
                        problems.push_back("The nested insert for \"" + b.target +
                                           "\" is not an array");
                    break;
                }
                const BlockDef* inner = doc.block(ni->block);
                const bool ok =
                    inner && ((inner->assembly && asmFind(*inner->assembly, b.target) >= 0) ||
                              (inner->dynamic && dynFind(*inner->dynamic, b.target) >= 0));
                if (!ok)
                    problems.push_back("The nested block has no parameter \"" + b.target + "\"");
                break;
            }
        }
    }
    return problems;
}

namespace {

std::size_t arrayItems(const InsertData& in) {
    if (!in.array)
        return 1;
    const ArrayInfo& a = *in.array;
    const long long n = a.kind == ArrayInfo::Kind::Rectangular
                            ? static_cast<long long>(a.rows) * a.columns
                            : static_cast<long long>(a.count);
    return static_cast<std::size_t>(std::max<long long>(n, 1));
}

std::string shownValue(const AsmParam& p, const AsmValue& v) {
    if (p.kind == AsmParam::Kind::Text || p.kind == AsmParam::Kind::Choice)
        return v.text;
    return expr::Value::ofNumber(v.number).toString();
}

void bomInsert(const Document& doc, const InsertData& in, std::size_t times, int depth,
               std::vector<BomRow>& rows) {
    const BlockDef* def = doc.block(in.block);
    if (!def || depth > 8)
        return;
    times *= arrayItems(in);
    if (def->assembly) {
        BomRow row;
        row.block = def->name;
        const auto eff = asmEffectiveValues(*def->assembly, in);
        for (std::size_t i = 0; i < def->assembly->params.size() && i < eff.size(); ++i)
            row.values.emplace_back(def->assembly->params[i].name,
                                    shownValue(def->assembly->params[i], eff[i]));
        auto same = std::find_if(rows.begin(), rows.end(), [&](const BomRow& r) {
            return r.block == row.block && r.values == row.values;
        });
        if (same != rows.end())
            same->count += times;
        else {
            row.count = times;
            rows.push_back(std::move(row));
        }
    }
    // inserts inside the block, with the values this insert's bindings give them
    std::vector<Entity> inner;
    for (EntityId id : def->entities)
        if (const Entity* e = doc.find(id))
            if (entityAs<InsertData>(*e))
                inner.push_back(*e);
    if (inner.empty())
        return;
    if (def->assembly)
        applyNested(doc, inner, resolveAssembly(doc, *def, in).nested);
    for (const Entity& e : inner)
        if (const auto* child = entityAs<InsertData>(e))
            bomInsert(doc, *child, times, depth + 1, rows);
}

}  // namespace

std::vector<BomRow> assemblyBom(const Document& doc) {
    std::vector<BomRow> rows;
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            if (const auto* in = entityAs<InsertData>(*e))
                bomInsert(doc, *in, 1, 0, rows);
    std::sort(rows.begin(), rows.end(), [](const BomRow& a, const BomRow& b) {
        return a.block != b.block ? a.block < b.block : a.values < b.values;
    });
    return rows;
}

std::string bomValuesText(const BomRow& r) {
    std::string t;
    for (const auto& [n, v] : r.values)
        t += (t.empty() ? "" : ", ") + n + "=" + v;
    return t;
}

std::string assemblyBomCsv(const std::vector<BomRow>& rows) {
    auto quote = [](const std::string& s) {
        if (s.find_first_of(",\"\r\n") == std::string::npos)
            return s;
        std::string q = "\"";
        for (char c : s) {
            if (c == '"')
                q += '"';
            q += c;
        }
        return q + "\"";
    };
    std::string out = "Assembly,Parameters,Quantity\r\n";
    for (const BomRow& r : rows)
        out +=
            quote(r.block) + "," + quote(bomValuesText(r)) + "," + std::to_string(r.count) + "\r\n";
    return out;
}

}  // namespace tbcad::blocks
