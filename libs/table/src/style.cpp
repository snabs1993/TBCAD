// Table styles (TABLESTYLE): named sets of sizes, kept in Header::custom so they are saved with the
// drawing.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "tbcad/table/table.h"

namespace tbcad::table {

namespace {
constexpr const char* kPrefix = "tbcad.tablestyle.";
constexpr const char* kCurrentKey =
    "tbcad.tablestyle.current";  // never a style name: has no '.' after

std::string keyFor(const std::string& name) {
    return std::string(kPrefix) + "s:" + name;
}
}  // namespace

std::string validStyleName(const std::string& raw) {
    std::string n;
    for (char c : raw)
        if (c != '|' && c != '\n' && c != '\t')
            n += c;
    while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back())))
        n.pop_back();
    std::size_t b = 0;
    while (b < n.size() && std::isspace(static_cast<unsigned char>(n[b])))
        ++b;
    n = n.substr(b);
    return n.size() > 255 ? n.substr(0, 255) : n;
}

std::vector<TableStyle> tableStyles(const Header& h) {
    std::vector<TableStyle> out;
    TableStyle standard;
    standard.name = "Standard";
    out.push_back(standard);
    const std::string prefix = std::string(kPrefix) + "s:";
    for (const auto& [key, value] : h.custom) {
        if (key.compare(0, prefix.size(), prefix) != 0)
            continue;
        TableStyle s;
        s.name = key.substr(prefix.size());
        std::vector<std::string> f;
        std::stringstream ss(value);
        for (std::string part; std::getline(ss, part, '|');)
            f.push_back(part);
        if (f.size() < 4)
            continue;
        const double cw = std::atof(f[0].c_str()), rh = std::atof(f[1].c_str()),
                     th = std::atof(f[2].c_str()), mg = std::atof(f[3].c_str());
        if (!(cw > 0) || !(rh > 0) || !(th > 0) || !(mg >= 0))
            continue;
        s.colWidth = cw;
        s.rowHeight = rh;
        s.textHeight = th;
        s.margin = mg;
        if (f.size() > 4)
            s.textStyle = f[4];
        if (s.name == "Standard")
            out[0] = s;  // a stored Standard replaces the built-in one
        else
            out.push_back(std::move(s));
    }
    return out;
}

std::optional<TableStyle> findTableStyle(const Header& h, const std::string& name) {
    for (TableStyle& s : tableStyles(h)) {
        if (s.name.size() == name.size() &&
            std::equal(s.name.begin(), s.name.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }))
            return std::move(s);
    }
    return std::nullopt;
}

void setTableStyle(Header& h, const TableStyle& s) {
    std::ostringstream v;
    v << s.colWidth << '|' << s.rowHeight << '|' << s.textHeight << '|' << s.margin << '|'
      << s.textStyle;
    // Replace a style of the same name whatever the case.
    for (auto it = h.custom.begin(); it != h.custom.end();) {
        const std::string prefix = std::string(kPrefix) + "s:";
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            const std::string n = it->first.substr(prefix.size());
            if (n.size() == s.name.size() &&
                std::equal(n.begin(), n.end(), s.name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                it = h.custom.erase(it);
                continue;
            }
        }
        ++it;
    }
    h.custom[keyFor(s.name)] = v.str();
}

bool deleteTableStyle(Header& h, const std::string& name) {
    auto found = findTableStyle(h, name);
    if (!found || found->name == "Standard")
        return false;
    h.custom.erase(keyFor(found->name));
    auto cur = h.custom.find(kCurrentKey);
    if (cur != h.custom.end() && cur->second == found->name)
        h.custom.erase(cur);
    return true;
}

std::string currentTableStyleName(const Header& h) {
    auto it = h.custom.find(kCurrentKey);
    if (it != h.custom.end() && findTableStyle(h, it->second))
        return findTableStyle(h, it->second)->name;
    return "Standard";
}

void setCurrentTableStyle(Header& h, const std::string& name) {
    h.custom[kCurrentKey] = name;
}

}  // namespace tbcad::table
