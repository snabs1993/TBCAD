// Dynamic blocks and assemblies in DXF (TBCAD XDATA). DXF has no form for them, so TBCAD keeps
// them in its own text, which other programs carry along untouched:
//   * on the BLOCK of the definition, "dyn:" / "asm:" chunks of the parameters, actions, bindings
//     and presets; the block's objects are named by their handles in the file;
//   * on each INSERT, "dynof=<block>" and "dynv:" chunks of its values. The INSERT itself names an
//     anonymous block of the objects as the values show them, so other programs draw the same.
// The text is lines of TAB-separated fields (backslash, TAB and line breaks escaped), cut into
// strings short enough for XDATA (dynXdataLines).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/tables.h"

namespace tbcad::dxf::detail {

inline std::string dynEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out.push_back(c);
    }
    return out;
}

inline std::string dynUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out.push_back(n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::string dynNum(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", std::isfinite(v) ? v : 0.0);
    return buf;
}

inline std::vector<std::string> dynSplit(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t j = s.find(sep, at);
        out.emplace_back(s.substr(at, j == std::string_view::npos ? s.size() - at : j - at));
        if (j == std::string_view::npos)
            break;
        at = j + 1;
    }
    return out;
}

// Chunks of at most `limit` bytes, cut at UTF-8 character boundaries.
inline std::vector<std::string> dynChunks(const std::string& text, std::size_t limit = 200) {
    std::vector<std::string> out;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t n = std::min(limit, text.size() - at);
        while (at + n < text.size() && n > 1 &&
               (static_cast<unsigned char>(text[at + n]) & 0xC0) == 0x80)
            --n;
        out.push_back(text.substr(at, n));
        at += n;
    }
    return out;
}

// XDATA strings cannot hold line breaks: each line of `text` goes out as `prefix` + ":" + its first
// chunk, then `prefix` + "+" + the rest.
inline std::vector<std::string> dynXdataLines(const std::string& text, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& line : dynSplit(text, '\n')) {
        if (line.empty())
            continue;
        bool first = true;
        for (const std::string& chunk : dynChunks(line)) {
            out.push_back(prefix + (first ? ":" : "+") + chunk);
            first = false;
        }
    }
    return out;
}

// Adds one XDATA string to the text being rebuilt; false when it is not one of `prefix`.
inline bool dynXdataAppend(std::string& text, std::string_view value, std::string_view prefix) {
    if (value.size() <= prefix.size() || value.substr(0, prefix.size()) != prefix)
        return false;
    const char mark = value[prefix.size()];
    if (mark != ':' && mark != '+')
        return false;
    if (text.size() >= (4u << 20))
        return true;
    if (mark == ':' && !text.empty())
        text += '\n';
    text += value.substr(prefix.size() + 1);
    return true;
}

using IdToRef = std::function<std::string(EntityId)>;
using RefToId = std::function<EntityId(std::string_view)>;

inline std::string dynIdList(const std::vector<EntityId>& ids, const IdToRef& ref) {
    std::string s;
    for (EntityId id : ids)
        s += (s.empty() ? "" : ",") + ref(id);
    return s;
}

inline std::vector<EntityId> dynParseIds(std::string_view s, const RefToId& id) {
    std::vector<EntityId> out;
    if (s.empty())
        return out;
    for (const std::string& part : dynSplit(s, ','))
        if (EntityId e = id(part))
            out.push_back(e);
    return out;
}

inline std::string asmValueText(const AsmValue& v) {
    return dynEscape(v.name) + "\t" + (v.isText ? "1" : "0") + "\t" + dynNum(v.number) + "\t" +
           dynEscape(v.text);
}

inline std::optional<AsmValue> asmValueParse(const std::vector<std::string>& f, std::size_t at) {
    if (f.size() < at + 4)
        return std::nullopt;
    AsmValue v;
    v.name = dynUnescape(f[at]);
    v.isText = f[at + 1] == "1";
    v.number = std::strtod(f[at + 2].c_str(), nullptr);
    v.text = dynUnescape(f[at + 3]);
    return v;
}

// The definition text of a block's dynamic parameters and assembly ("" when it has neither).
inline std::string blockExtrasText(const BlockDef& def, const IdToRef& ref) {
    std::string t;
    auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& f : fields) {
            if (!first)
                t += '\t';
            t += f;
            first = false;
        }
        t += '\n';
    };
    if (def.dynamic) {
        for (const DynParam& p : def.dynamic->params) {
            line({"P", std::to_string(static_cast<int>(p.kind)), dynEscape(p.name),
                  dynNum(p.base.x), dynNum(p.base.y), dynNum(p.end.x), dynNum(p.end.y),
                  dynNum(p.min), dynNum(p.max)});
            for (std::size_t i = 0; i < p.states.size(); ++i)
                line({"S", dynEscape(p.states[i]),
                      i < p.stateObjects.size() ? dynIdList(p.stateObjects[i], ref) : ""});
            if (p.kind == DynParam::Kind::Lookup) {
                std::string params;
                for (int k : p.lookupParams)
                    params += (params.empty() ? "" : ",") + std::to_string(k);
                line({"L", params});
                for (const auto& row : p.lookupValues) {
                    std::string values;
                    for (double v : row)
                        values += (values.empty() ? "" : ",") + dynNum(v);
                    line({"R", values});
                }
            }
        }
        for (const DynAction& a : def.dynamic->actions)
            line({"A", std::to_string(static_cast<int>(a.kind)), std::to_string(a.param),
                  dynNum(a.frameMin.x), dynNum(a.frameMin.y), dynNum(a.frameMax.x),
                  dynNum(a.frameMax.y), dynIdList(a.objects, ref)});
    }
    if (def.assembly) {
        line({"Y"});  // the block is an assembly (it may have no parameters yet)
        for (const AsmParam& p : def.assembly->params) {
            line({"Q", std::to_string(static_cast<int>(p.kind)), dynEscape(p.name),
                  dynNum(p.defaultNumber), dynEscape(p.defaultText), dynNum(p.min), dynNum(p.max)});
            for (const std::string& c : p.choices)
                line({"C", dynEscape(c)});
        }
        for (const AsmBinding& b : def.assembly->bindings)
            line({"B", std::to_string(static_cast<int>(b.kind)), b.entity ? ref(b.entity) : "0",
                  dynEscape(b.target), dynEscape(b.formula)});
        for (const AsmPreset& pr : def.assembly->presets) {
            line({"G", dynEscape(pr.name)});
            for (const AsmValue& v : pr.values)
                t += "V\t" + asmValueText(v) + "\n";
        }
        if (!def.assembly->joinClass.empty())
            line({"J", dynEscape(def.assembly->joinClass)});
    }
    return t;
}

// Reads blockExtrasText back into `def`. Unknown lines are skipped; a kind out of range drops the
// line it is on.
inline void parseBlockExtras(const std::string& text, BlockDef& def, const RefToId& id) {
    DynamicDef dyn;
    AssemblyDef as;
    bool anyDyn = false, anyAsm = false;
    for (const std::string& raw : dynSplit(text, '\n')) {
        if (raw.empty())
            continue;
        const std::vector<std::string> f = dynSplit(raw, '\t');
        const std::string& tag = f[0];
        const auto num = [&](std::size_t i) {
            return i < f.size() ? std::strtod(f[i].c_str(), nullptr) : 0.0;
        };
        const auto kind = [&](int max) {
            const int k = f.size() > 1 ? std::atoi(f[1].c_str()) : -1;
            return k >= 0 && k <= max ? k : -1;
        };
        if (tag == "P" && f.size() >= 9) {
            const int k = kind(static_cast<int>(DynParam::Kind::Lookup));
            if (k < 0)
                continue;
            DynParam p;
            p.kind = static_cast<DynParam::Kind>(k);
            p.name = dynUnescape(f[2]);
            p.base = {num(3), num(4)};
            p.end = {num(5), num(6)};
            p.min = num(7);
            p.max = num(8);
            dyn.params.push_back(std::move(p));
            anyDyn = true;
        } else if (tag == "S" && f.size() >= 3 && !dyn.params.empty()) {
            DynParam& p = dyn.params.back();
            p.states.push_back(dynUnescape(f[1]));
            p.stateObjects.push_back(dynParseIds(f[2], id));
        } else if (tag == "L" && f.size() >= 2 && !dyn.params.empty()) {
            if (!f[1].empty())
                for (const std::string& k : dynSplit(f[1], ','))
                    dyn.params.back().lookupParams.push_back(std::atoi(k.c_str()));
        } else if (tag == "R" && f.size() >= 2 && !dyn.params.empty()) {
            std::vector<double> row;
            if (!f[1].empty())
                for (const std::string& v : dynSplit(f[1], ','))
                    row.push_back(std::strtod(v.c_str(), nullptr));
            dyn.params.back().lookupValues.push_back(std::move(row));
        } else if (tag == "A" && f.size() >= 8) {
            const int k = kind(static_cast<int>(DynAction::Kind::Array));
            if (k < 0)
                continue;
            DynAction a;
            a.kind = static_cast<DynAction::Kind>(k);
            a.param = std::atoi(f[2].c_str());
            a.frameMin = {num(3), num(4)};
            a.frameMax = {num(5), num(6)};
            a.objects = dynParseIds(f[7], id);
            dyn.actions.push_back(std::move(a));
            anyDyn = true;
        } else if (tag == "Y") {
            anyAsm = true;
        } else if (tag == "Q" && f.size() >= 7) {
            const int k = kind(static_cast<int>(AsmParam::Kind::Choice));
            if (k < 0)
                continue;
            AsmParam p;
            p.kind = static_cast<AsmParam::Kind>(k);
            p.name = dynUnescape(f[2]);
            p.defaultNumber = num(3);
            p.defaultText = dynUnescape(f[4]);
            p.min = num(5);
            p.max = num(6);
            as.params.push_back(std::move(p));
            anyAsm = true;
        } else if (tag == "C" && f.size() >= 2 && !as.params.empty()) {
            as.params.back().choices.push_back(dynUnescape(f[1]));
        } else if (tag == "B" && f.size() >= 5) {
            const int k = kind(static_cast<int>(AsmBinding::Kind::ArrayField));
            if (k < 0)
                continue;
            AsmBinding b;
            b.kind = static_cast<AsmBinding::Kind>(k);
            b.entity = f[2] == "0" ? EntityId{} : id(f[2]);
            b.target = dynUnescape(f[3]);
            b.formula = dynUnescape(f[4]);
            as.bindings.push_back(std::move(b));
            anyAsm = true;
        } else if (tag == "G" && f.size() >= 2) {
            as.presets.push_back({dynUnescape(f[1]), {}});
            anyAsm = true;
        } else if (tag == "V" && !as.presets.empty()) {
            if (auto v = asmValueParse(f, 1))
                as.presets.back().values.push_back(std::move(*v));
        } else if (tag == "J" && f.size() >= 2) {
            as.joinClass = dynUnescape(f[1]);
            anyAsm = true;
        }
    }
    if (anyDyn)
        def.dynamic = std::move(dyn);
    if (anyAsm)
        def.assembly = std::move(as);
}

// The values text of an insert of a dynamic block or assembly.
inline std::string insertValuesText(const InsertData& d) {
    std::string t;
    if (!d.dynValues.empty()) {
        t += "D";
        for (double v : d.dynValues)
            t += "\t" + dynNum(v);
        t += "\n";
    }
    for (const AsmValue& v : d.asmValues)
        t += "V\t" + asmValueText(v) + "\n";
    if (d.joinOff)
        t += "O\n";
    return t;
}

inline void parseInsertValues(const std::string& text, InsertData& d) {
    for (const std::string& raw : dynSplit(text, '\n')) {
        if (raw.empty())
            continue;
        const std::vector<std::string> f = dynSplit(raw, '\t');
        if (f[0] == "D") {
            d.dynValues.clear();
            for (std::size_t i = 1; i < f.size(); ++i)
                d.dynValues.push_back(std::strtod(f[i].c_str(), nullptr));
        } else if (f[0] == "V") {
            if (auto v = asmValueParse(f, 1))
                d.asmValues.push_back(std::move(*v));
        } else if (f[0] == "O") {
            d.joinOff = true;
        }
    }
}

}  // namespace tbcad::dxf::detail
