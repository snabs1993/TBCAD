#include "tbcad/mline/mline.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "tbcad/geom/geom.h"

namespace tbcad::mline {

namespace {

const std::string kPrefix = "mlstyle.";

std::string upper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string num(double v) {
    std::ostringstream o;
    o.precision(17);
    o << v;
    return o.str();
}

// "key=value" lines; description may contain anything but a newline.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            out.push_back(line);
    return out;
}

}  // namespace

Style standardStyle() {
    return Style{};
}

std::string encode(const Style& s) {
    std::string out = "desc=" + s.description + "\n";
    out += std::string("caps=") + (s.startCap ? "1" : "0") + (s.endCap ? "1" : "0") + "\n";
    out += "offsets=";
    for (std::size_t i = 0; i < s.offsets.size(); ++i)
        out += (i ? "," : "") + num(s.offsets[i]);
    out += "\n";
    return out;
}

std::optional<Style> decode(const std::string& name, const std::string& text) {
    Style s;
    s.name = upper(name);
    s.offsets.clear();
    for (const std::string& line : lines(text)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "desc") {
            s.description = val;
        } else if (key == "caps" && val.size() >= 2) {
            s.startCap = val[0] == '1';
            s.endCap = val[1] == '1';
        } else if (key == "offsets") {
            std::istringstream in(val);
            std::string item;
            while (std::getline(in, item, ',')) {
                try {
                    const double v = std::stod(item);
                    if (std::isfinite(v))
                        s.offsets.push_back(v);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
    }
    if (s.offsets.empty() || s.offsets.size() > 16)
        return std::nullopt;
    return s;
}

std::optional<Style> findStyle(const Document& doc, const std::string& name) {
    const std::string key = kPrefix + upper(name);
    const auto& custom = doc.header().custom;
    if (auto it = custom.find(key); it != custom.end())
        return decode(name, it->second);
    if (upper(name) == "STANDARD")
        return standardStyle();
    return std::nullopt;
}

std::vector<std::string> styleNames(const Document& doc) {
    std::vector<std::string> names{"STANDARD"};
    for (const auto& [k, v] : doc.header().custom)
        if (k.compare(0, kPrefix.size(), kPrefix) == 0) {
            const std::string n = k.substr(kPrefix.size());
            if (std::find(names.begin(), names.end(), n) == names.end())
                names.push_back(n);
        }
    return names;
}

void storeStyle(Header& header, const Style& style) {
    header.custom[kPrefix + upper(style.name)] = encode(style);
}

bool removeStyle(Header& header, const std::string& name) {
    return header.custom.erase(kPrefix + upper(name)) > 0;
}

Strokes strokes(const MlineData& d, const Style& style) {
    Strokes out;
    // Drop repeated points; fewer than two distinct points draw nothing.
    std::vector<Vec2> p;
    for (const Vec2& v : d.v)
        if (p.empty() || distance(p.back(), v) > 1e-12)
            p.push_back(v);
    bool closed = d.closed;
    if (closed && p.size() > 2 && distance(p.front(), p.back()) <= 1e-12)
        p.pop_back();
    if (p.size() < 2)
        return out;
    if (p.size() < 3)
        closed = false;
    const std::size_t n = p.size();

    // Left normal of each segment.
    const std::size_t segs = closed ? n : n - 1;
    std::vector<Vec2> normal(segs);
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 dir = normalized(p[(i + 1) % n] - p[i]);
        normal[i] = {-dir.y, dir.x};
    }
    // Mitre vector at each vertex: the offset point is vertex + offset * miter.
    std::vector<Vec2> miter(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool first = !closed && i == 0, last = !closed && i == n - 1;
        if (first) {
            miter[i] = normal[0];
        } else if (last) {
            miter[i] = normal[segs - 1];
        } else {
            const Vec2 a = normal[(i + segs - 1) % segs], b = normal[i % segs];
            const double k = 1.0 + dot(a, b);
            if (k < 0.1) {  // nearly a U-turn: a mitre would shoot off, so cut it square
                miter[i] = a;
            } else {
                miter[i] = {(a.x + b.x) / k, (a.y + b.y) / k};
            }
        }
    }
    double shift = 0;
    if (d.justification == 0)
        shift = -*std::max_element(style.offsets.begin(), style.offsets.end());
    else if (d.justification == 2)
        shift = -*std::min_element(style.offsets.begin(), style.offsets.end());
    for (double off : style.offsets) {
        const double o = (off + shift) * d.scale;
        std::vector<Vec2> line;
        line.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            line.push_back(p[i] + miter[i] * o);
        out.lines.push_back(std::move(line));
        out.closed.push_back(closed);
    }
    if (!closed && out.lines.size() >= 2) {
        // A cap runs across the end, from the lowest offset to the highest.
        auto cap = [&](std::size_t idx) {
            std::vector<std::pair<double, Vec2>> ends;
            for (std::size_t i = 0; i < style.offsets.size(); ++i)
                ends.emplace_back(style.offsets[i], out.lines[i][idx]);
            std::sort(ends.begin(), ends.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            out.lines.push_back({ends.front().second, ends.back().second});
            out.closed.push_back(false);
        };
        if (style.startCap)
            cap(0);
        if (style.endCap)
            cap(n - 1);
    }
    return out;
}

Strokes strokes(const Document& doc, const MlineData& d) {
    auto style = findStyle(doc, d.style);
    return strokes(d, style ? *style : standardStyle());
}

Result<std::vector<Entity>> explodeMline(const Document& doc, const Entity& e) {
    const auto* d = entityAs<MlineData>(e);
    if (!d)
        return makeError(ErrorCode::InvalidArgument, "not a multiline");
    const Strokes s = strokes(doc, *d);
    std::vector<Entity> out;
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        PolylineData pl;
        pl.closed = s.closed[i];
        for (const Vec2& v : s.lines[i]) {
            PolyVertex pv;
            pv.p = v;
            pl.v.push_back(pv);
        }
        Entity n;
        n.common = e.common;
        n.common.id = {};
        n.data = std::move(pl);
        out.push_back(std::move(n));
    }
    return out;
}

// DXF strings cannot hold line breaks, so the text form travels with \n and \\ escaped.
std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string serialize(const MlineData& d, const Style& style) {
    std::string out = "tbcad-mline\n";
    out += "name=" + d.style + "\n";
    out += "scale=" + num(d.scale) + "\n";
    out += "just=" + std::to_string(d.justification) + "\n";
    out += std::string("closed=") + (d.closed ? "1" : "0") + "\n";
    out += "v=";
    for (std::size_t i = 0; i < d.v.size(); ++i)
        out += (i ? ";" : "") + num(d.v[i].x) + "," + num(d.v[i].y);
    out += "\n";
    out += "style\n" + encode(style);
    return escape(out);
}

std::optional<Parsed> parse(const std::string& packed) {
    const std::string text = unescape(packed);
    if (text.rfind("tbcad-mline", 0) != 0)
        return std::nullopt;
    Parsed p;
    const auto at = text.find("\nstyle\n");
    if (at == std::string::npos)
        return std::nullopt;
    for (const std::string& line : lines(text.substr(0, at))) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        try {
            if (key == "name") {
                p.data.style = val;
            } else if (key == "scale") {
                p.data.scale = std::stod(val);
            } else if (key == "just") {
                p.data.justification = std::stoi(val);
            } else if (key == "closed") {
                p.data.closed = val == "1";
            } else if (key == "v") {
                std::istringstream in(val);
                std::string item;
                while (std::getline(in, item, ';')) {
                    const auto comma = item.find(',');
                    if (comma == std::string::npos)
                        return std::nullopt;
                    p.data.v.push_back(
                        {std::stod(item.substr(0, comma)), std::stod(item.substr(comma + 1))});
                }
            }
        } catch (...) {
            return std::nullopt;
        }
    }
    auto style = decode(p.data.style, text.substr(at + 7));
    if (!style)
        return std::nullopt;
    p.style = *style;
    return p;
}

}  // namespace tbcad::mline
