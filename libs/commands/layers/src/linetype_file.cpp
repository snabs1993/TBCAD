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

LinetypeFile parseLinetypes(std::string_view text) {
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
            auto it = std::find_if(file.linetypes.begin(), file.linetypes.end(),
                                   [&](const Linetype& lt) {
                                       return lowerAscii(lt.name) == lowerAscii(n);
                                   });
            if (it == file.linetypes.end())
                return makeError(ErrorCode::NotFound, "Linetype \"" + n + "\" is not in the file.");
            chosen.push_back(&*it);
        }
    }
    std::vector<std::string> done;
    for (const Linetype* lt : chosen) {
        const Linetype* have = doc.linetypeByName(lt->name);
        if (have) {
            if (mode == LoadMode::SkipExisting || have->pattern == lt->pattern)
                continue;
            Linetype copy = *lt;
            auto r = tx.modifyLinetype(have->id, [&](Linetype& row) {
                row.description = copy.description;
                row.pattern = copy.pattern;
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
