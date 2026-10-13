#include "tbcad/text/field.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "tbcad/geom/entity.h"

namespace tbcad::text {

namespace {

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        std::size_t colon = s.find(':', at);
        out.emplace_back(s.substr(at, colon == std::string_view::npos ? colon : colon - at));
        if (colon == std::string_view::npos)
            break;
        at = colon + 1;
    }
    return out;
}

std::string todayText(std::string_view format) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::string f = format.empty() ? std::string("yyyy-MM-dd") : std::string(format);
    auto two = [](int v) {
        char b[8];
        std::snprintf(b, sizeof b, "%02d", v);
        return std::string(b);
    };
    auto replaceAll = [](std::string& s, const std::string& what, const std::string& with) {
        for (std::size_t at = s.find(what); at != std::string::npos;
             at = s.find(what, at + with.size()))
            s.replace(at, what.size(), with);
    };
    replaceAll(f, "BBBB", std::to_string(tm.tm_year + 1900 + 543));
    replaceAll(f, "yyyy", std::to_string(tm.tm_year + 1900));
    replaceAll(f, "MM", two(tm.tm_mon + 1));
    replaceAll(f, "dd", two(tm.tm_mday));
    replaceAll(f, "HH", two(tm.tm_hour));
    replaceAll(f, "mm", two(tm.tm_min));
    return f;
}

std::string baseName(const std::string& path, bool full) {
    if (full)
        return path;
    std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

const Entity* byRef(const Document& doc, const std::string& ref) {
    if (ref.size() < 2 || ref[0] != '#')
        return nullptr;
    char* end = nullptr;
    unsigned long long v = std::strtoull(ref.c_str() + 1, &end, 10);
    if (end == ref.c_str() + 1 || *end != 0)
        return nullptr;
    return doc.find(EntityId{v});
}

// Divisor from drawing units to the unit asked for; drawings are in millimetres unless the
// unit is changed, which the field does not try to follow.
bool unitDivisor(const std::vector<std::string>& args, std::size_t from, int power, double& div) {
    div = 1;
    if (args.size() <= from)
        return true;
    const std::string& u = args[from];
    double f = u == "m" ? 1000.0 : u == "cm" ? 10.0 : u == "mm" ? 1.0 : 0.0;
    if (f == 0.0)
        return false;
    div = std::pow(f, power);
    return true;
}

std::string number(double v, int decimals) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

std::string evaluate(const Document& doc, std::string_view body, std::string_view today) {
    const std::vector<std::string> a = split(body);
    const std::string& name = a[0];
    auto decimalsAt = [&](std::size_t i, int def) {
        if (a.size() > i) {
            char* end = nullptr;
            long d = std::strtol(a[i].c_str(), &end, 10);
            if (end != a[i].c_str() && *end == 0 && d >= 0 && d <= 12)
                return static_cast<int>(d);
        }
        return def;
    };
    if (name == "date")
        return today.empty() ? todayText(a.size() > 1 ? std::string_view(body).substr(5) : "")
                             : std::string(today);
    const auto& custom = doc.header().custom;
    if (name == "filename" || name == "filepath") {
        if (doc.filePath().empty())
            return "####";
        return baseName(doc.filePath(), name == "filepath");
    }
    if (name == "sheet") {
        auto it = custom.find("sheet.number");
        return it == custom.end() ? "####" : it->second;
    }
    if (name == "var" && a.size() > 1) {
        auto it = custom.find(a[1]);
        return it == custom.end() ? "####" : it->second;
    }
    if (a.size() > 1 && (name == "length" || name == "area" || name == "text")) {
        const Entity* e = byRef(doc, a[1]);
        if (!e)
            return "####";
        if (name == "text") {
            if (const auto* t = entityAs<TextData>(*e))
                return t->utf8;
            if (const auto* m = entityAs<MTextData>(*e))
                return m->utf8;
            return "####";
        }
        double div = 1;
        if (name == "length") {
            if (!unitDivisor(a, 3, 1, div))
                return "####";
            const double len = geom::length(*e);
            return len > 0 ? number(len / div, decimalsAt(2, 2)) : "####";
        }
        if (!unitDivisor(a, 3, 2, div))
            return "####";
        double area = -1;
        if (const auto* c = entityAs<CircleData>(*e))
            area = kPi * c->radius * c->radius;
        else if (const auto* p = entityAs<PolylineData>(*e); p && p->closed)
            area = geom::area(*p);
        return area >= 0 ? number(area / div, decimalsAt(2, 2)) : "####";
    }
    return "####";
}

}  // namespace

bool hasFields(std::string_view s) {
    return s.find("%<") != std::string_view::npos && s.find(">%") != std::string_view::npos;
}

std::string expandFields(const Document& doc, std::string_view s, std::string_view today) {
    if (!hasFields(s))
        return std::string(s);
    std::string out;
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t open = s.find("%<", at);
        std::size_t close = open == std::string_view::npos ? open : s.find(">%", open + 2);
        if (open == std::string_view::npos || close == std::string_view::npos) {
            out.append(s.substr(at));
            break;
        }
        out.append(s.substr(at, open - at));
        out += evaluate(doc, s.substr(open + 2, close - open - 2), today);
        at = close + 2;
    }
    return out;
}

}  // namespace tbcad::text
