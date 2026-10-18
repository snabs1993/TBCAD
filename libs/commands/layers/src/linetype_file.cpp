#include "tbcad/layers/linetype_file.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace tbcad::layers {

namespace {

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

namespace {

bool isAcadLin(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trimmed(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        return line.front() == '*';
    }
    return false;
}

// Splits at commas that are outside quotes and brackets.
std::vector<std::string> splitTop(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool quoted = false;
    for (char c : s) {
        if (c == '"')
            quoted = !quoted;
        if (!quoted && c == '[')
            ++depth;
        if (!quoted && c == ']')
            --depth;
        if (c == ',' && depth == 0 && !quoted) {
            out.push_back(trimmed(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trimmed(cur));
    return out;
}

bool toNumber(std::string_view tok, double& v) {
    const std::string t = trimmed(tok);
    if (t.empty())
        return false;
    // from_chars wants no leading '+', and files write ".5" and "-.25".
    const char* b = t.data();
    const char* e = t.data() + t.size();
    if (*b == '+')
        ++b;
    auto r = std::from_chars(b, e, v);
    return r.ec == std::errc() && r.ptr == e && std::isfinite(v);
}

// "R=90" style option of a bracketed element. Angles are degrees.
bool applyOption(LinetypeMark& m, const std::string& opt) {
    const std::size_t eq = opt.find('=');
    if (eq == std::string::npos)
        return false;
    const std::string key = lowerAscii(trimmed(std::string_view(opt).substr(0, eq)));
    double v = 0;
    if (!toNumber(std::string_view(opt).substr(eq + 1), v))
        return false;
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    if (key == "s") {
        m.scale = v;
    } else if (key == "r") {
        m.rotation = v * kDeg;
        m.rotationKind = LinetypeMark::Rotation::Relative;
    } else if (key == "a") {
        m.rotation = v * kDeg;
        m.rotationKind = LinetypeMark::Rotation::Absolute;
    } else if (key == "u") {
        m.rotation = v * kDeg;
        m.rotationKind = LinetypeMark::Rotation::Upright;
    } else if (key == "x") {
        m.shift.x = v;
    } else if (key == "y") {
        m.shift.y = v;
    } else {
        return false;
    }
    return true;
}

// [ "TEXT" | SHAPE , style | shapefile , S=.. R=.. X=.. Y=.. ]
bool parseMark(const std::string& tok, LinetypeMark& m, std::string& why) {
    if (tok.size() < 2 || tok.front() != '[' || tok.back() != ']') {
        why = "unterminated [element]";
        return false;
    }
    const std::vector<std::string> parts = splitTop(std::string_view(tok).substr(1, tok.size() - 2));
    if (parts.empty() || parts[0].empty()) {
        why = "empty [element]";
        return false;
    }
    const std::string& head = parts[0];
    if (head.front() == '"') {
        if (head.size() < 2 || head.back() != '"') {
            why = "text is missing its closing quote";
            return false;
        }
        m.kind = LinetypeMark::Kind::Text;
        m.text = head.substr(1, head.size() - 2);
    } else {
        m.kind = LinetypeMark::Kind::Shape;
        m.text = head;
    }
    if (parts.size() >= 2)
        m.style = parts[1];
    for (std::size_t i = 2; i < parts.size(); ++i)
        if (!parts[i].empty() && !applyOption(m, parts[i])) {
            why = "cannot read \"" + parts[i] + "\"";
            return false;
        }
    // Shapes always have a size; without S= the shape file's own size is 1.
    if (m.kind == LinetypeMark::Kind::Shape && m.scale == 0)
        m.scale = 1;
    return true;
}

LinetypeFile parseAcadLin(std::string_view text) {
    LinetypeFile out;
    std::istringstream in{std::string(text)};
    std::string raw;
    int lineNo = 0;
    auto problem = [&](int at, const std::string& msg) {
        out.problems.push_back("line " + std::to_string(at) + ": " + msg);
    };
    Linetype pending;
    bool havePending = false;
    int headerLine = 0;
    auto finish = [&](const std::string& patternLine, int at) {
        Linetype lt = std::move(pending);
        havePending = false;
        const std::vector<std::string> toks = splitTop(patternLine);
        if (toks.empty() || lowerAscii(toks[0]) != "a") {
            problem(at, "\"" + lt.name + "\": the pattern line must start with A");
            return;
        }
        double offset = 0;
        for (std::size_t i = 1; i < toks.size(); ++i) {
            const std::string& tok = toks[i];
            if (tok.empty())
                continue;
            if (tok.front() == '[') {
                LinetypeMark m;
                std::string why;
                if (!parseMark(tok, m, why)) {
                    problem(at, "\"" + lt.name + "\": " + why);
                    return;
                }
                m.offset = offset;
                lt.marks.push_back(std::move(m));
                continue;
            }
            double v = 0;
            if (!toNumber(tok, v)) {
                problem(at, "\"" + tok + "\" is not a number");
                return;
            }
            lt.pattern.push_back(v);
            offset += std::fabs(v);
        }
        if (lowerAscii(lt.name) == "continuous" || lowerAscii(lt.name) == "bylayer" ||
            lowerAscii(lt.name) == "byblock") {
            return;  // the standard file lists CONTINUOUS-like entries nobody can redefine
        }
        if (offset <= 0) {
            problem(at, "\"" + lt.name + "\" pattern has no length");
            return;
        }
        const bool dup = std::any_of(out.linetypes.begin(), out.linetypes.end(), [&](const auto& o) {
            return lowerAscii(o.name) == lowerAscii(lt.name);
        });
        if (dup) {
            problem(at, "\"" + lt.name + "\" is defined twice");
            return;
        }
        out.linetypes.push_back(std::move(lt));
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = trimmed(raw);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        if (line.front() == '*') {
            if (havePending)
                problem(headerLine, "\"" + pending.name + "\" has no pattern line");
            const std::size_t comma = line.find(',');
            pending = Linetype{};
            pending.name = trimmed(std::string_view(line).substr(
                1, comma == std::string::npos ? std::string::npos : comma - 1));
            if (comma != std::string::npos)
                pending.description = trimmed(std::string_view(line).substr(comma + 1));
            havePending = true;
            headerLine = lineNo;
            if (pending.name.empty()) {
                problem(lineNo, "linetype name is empty");
                havePending = false;
            }
            continue;
        }
        if (!havePending) {
            problem(lineNo, "pattern line without a *NAME line");
            continue;
        }
        finish(line, lineNo);
    }
    if (havePending)
        problem(headerLine, "\"" + pending.name + "\" has no pattern line");
    return out;
}

}  // namespace

LinetypeFile parseLinetypes(std::string_view text) {
    if (isAcadLin(text))
        return parseAcadLin(text);
    LinetypeFile out;
    std::istringstream in{std::string(text)};
    std::string raw;
    int lineNo = 0;
    auto problem = [&](const std::string& msg) {
        out.problems.push_back("line " + std::to_string(lineNo) + ": " + msg);
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = trimmed(raw);
        if (line.empty() || line.front() == '#')
            continue;
        std::size_t a = line.find(';');
        std::size_t b = a == std::string::npos ? a : line.find(';', a + 1);
        if (b == std::string::npos) {
            problem("expected \"name ; description ; pattern\"");
            continue;
        }
        Linetype lt;
        lt.name = trimmed(std::string_view(line).substr(0, a));
        lt.description = trimmed(std::string_view(line).substr(a + 1, b - a - 1));
        if (lt.name.empty()) {
            problem("linetype name is empty");
            continue;
        }
        if (lowerAscii(lt.name) == "continuous" || lowerAscii(lt.name) == "bylayer" ||
            lowerAscii(lt.name) == "byblock" || lowerAscii(lt.name) == "byparent") {
            problem("\"" + lt.name + "\" is a reserved name");
            continue;
        }
        std::string pat = line.substr(b + 1);
        std::replace(pat.begin(), pat.end(), ',', ' ');
        std::istringstream ps(pat);
        std::string tok;
        bool bad = false;
        while (ps >> tok) {
            double v = 0;
            auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
            if (r.ec != std::errc() || r.ptr != tok.data() + tok.size() || !std::isfinite(v)) {
                problem("\"" + tok + "\" is not a number");
                bad = true;
                break;
            }
            lt.pattern.push_back(v);
        }
        if (bad)
            continue;
        if (lt.pattern.empty()) {
            problem("\"" + lt.name + "\" has no pattern");
            continue;
        }
        double period = 0;
        for (double v : lt.pattern)
            period += std::fabs(v);
        if (period <= 0) {
            problem("\"" + lt.name + "\" pattern has no length");
            continue;
        }
        bool dup = std::any_of(out.linetypes.begin(), out.linetypes.end(), [&](const Linetype& o) {
            return lowerAscii(o.name) == lowerAscii(lt.name);
        });
        if (dup) {
            problem("\"" + lt.name + "\" is defined twice");
            continue;
        }
        out.linetypes.push_back(std::move(lt));
    }
    return out;
}

Result<std::vector<std::string>> loadLinetypes(const Document& doc, Transaction& tx,
                                               const LinetypeFile& file,
                                               const std::vector<std::string>& names,
                                               LoadMode mode) {
    bool all = names.empty() || (names.size() == 1 && names[0] == "*");
    std::vector<const Linetype*> chosen;
    if (all) {
        for (const Linetype& lt : file.linetypes)
            chosen.push_back(&lt);
    } else {
        for (const std::string& n : names) {
            auto it = std::find_if(
                file.linetypes.begin(), file.linetypes.end(),
                [&](const Linetype& lt) { return lowerAscii(lt.name) == lowerAscii(n); });
            if (it == file.linetypes.end())
                return makeError(ErrorCode::NotFound, "Linetype \"" + n + "\" is not in the file.");
            chosen.push_back(&*it);
        }
    }
    std::vector<std::string> done;
    for (const Linetype* lt : chosen) {
        const Linetype* have = doc.linetypeByName(lt->name);
        if (have) {
            if (mode == LoadMode::SkipExisting ||
                (have->pattern == lt->pattern && have->marks == lt->marks))
                continue;
            Linetype copy = *lt;
            auto r = tx.modifyLinetype(have->id, [&](Linetype& row) {
                row.description = copy.description;
                row.pattern = copy.pattern;
                row.marks = copy.marks;
            });
            if (!r)
                return r.error();
        } else {
            Linetype row = *lt;
            row.id = {};
            auto r = tx.addLinetype(std::move(row));
            if (!r)
                return r.error();
        }
        done.push_back(lt->name);
    }
    return done;
}

Result<LinetypeFile> readLinetypeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::Io, "Cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseLinetypes(ss.str());
}

}  // namespace tbcad::layers
