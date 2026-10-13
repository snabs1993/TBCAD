#include "tbcad/drafting/ucs.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace tbcad::drafting {

namespace {

constexpr const char* kHeaderKey = "tbcad.ucs";

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

Result<void> store(Transaction& tx, const std::vector<Ucs>& all) {
    std::ostringstream o;
    for (const Ucs& u : all)
        o << u.name << '\t' << num(u.origin.x) << '\t' << num(u.origin.y) << '\t' << num(u.angle)
          << '\n';
    std::string text = o.str();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kHeaderKey);
        else
            h.custom[kHeaderKey] = text;
    });
}

}  // namespace

Vec2 Ucs::dirToWorld(Vec2 v) const {
    const double c = std::cos(angle), s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

Vec2 Ucs::dirFromWorld(Vec2 v) const {
    const double c = std::cos(angle), s = std::sin(angle);
    return {v.x * c + v.y * s, -v.x * s + v.y * c};
}

Vec2 Ucs::toWorld(Vec2 p) const {
    return origin + dirToWorld(p);
}

Vec2 Ucs::fromWorld(Vec2 p) const {
    return dirFromWorld(p - origin);
}

std::vector<Ucs> ucsList(const Document& doc) {
    std::vector<Ucs> out;
    auto it = doc.header().custom.find(kHeaderKey);
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& line : split(it->second, '\n')) {
        auto f = split(line, '\t');
        if (f.size() != 4 || f[0].empty())
            continue;
        Ucs u;
        u.name = f[0];
        u.origin = {std::strtod(f[1].c_str(), nullptr), std::strtod(f[2].c_str(), nullptr)};
        u.angle = std::strtod(f[3].c_str(), nullptr);
        out.push_back(std::move(u));
    }
    return out;
}

Result<void> validateUcsName(const std::string& name) {
    if (name.empty() || name.size() > 31)
        return Error{ErrorCode::InvalidArgument, "A UCS name has 1 to 31 characters"};
    if (name.find_first_of("\t,\r\n") != std::string::npos)
        return Error{ErrorCode::InvalidArgument,
                     "A UCS name cannot contain tabs, commas or line breaks"};
    if (sameName(name, kWorldUcsName))
        return Error{ErrorCode::InvalidArgument, "\"World\" is the built-in system"};
    return {};
}

Result<void> saveUcs(Transaction& tx, const Ucs& ucs) {
    if (auto v = validateUcsName(ucs.name); !v)
        return v.error();
    std::vector<Ucs> all = ucsList(tx.document());
    auto it = std::find_if(all.begin(), all.end(),
                           [&](const Ucs& u) { return sameName(u.name, ucs.name); });
    if (it != all.end())
        *it = ucs;
    else
        all.push_back(ucs);
    return store(tx, all);
}

Result<void> deleteUcs(Transaction& tx, const std::string& name) {
    std::vector<Ucs> all = ucsList(tx.document());
    auto it =
        std::find_if(all.begin(), all.end(), [&](const Ucs& u) { return sameName(u.name, name); });
    if (it == all.end())
        return Error{ErrorCode::NotFound, "No UCS named \"" + name + "\""};
    all.erase(it);
    return store(tx, all);
}

Ucs currentUcs(const Document* doc, const cmd::IHost* host) {
    if (!doc || !host)
        return {};
    auto name = host->setting(kUcsSettingKey);
    if (!name || name->empty() || sameName(*name, kWorldUcsName))
        return {};
    if ((*name)[0] == '@') {  // an unnamed system: "@x,y,angleDegrees"
        double v[3] = {0, 0, 0};
        const char* c = name->c_str() + 1;
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            v[i] = std::strtod(c, &end);
            if (end == c)
                return {};
            c = (*end == ',') ? end + 1 : end;
        }
        Ucs u;
        u.origin = {v[0], v[1]};
        u.angle = v[2] * kPi / 180.0;
        return u;
    }
    for (const Ucs& u : ucsList(*doc))
        if (sameName(u.name, *name))
            return u;
    return {};
}

std::string unnamedUcsSetting(const Ucs& ucs) {
    char buf[120];
    std::snprintf(buf, sizeof buf, "@%.12g,%.12g,%.12g", ucs.origin.x, ucs.origin.y,
                  ucs.angle * 180.0 / kPi);
    return buf;
}

}  // namespace tbcad::drafting
