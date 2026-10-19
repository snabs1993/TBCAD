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
    if (s.startArc || s.endArc)
        out += "arcs=" + std::to_string(s.startArc) + std::to_string(s.endArc) + "\n";
    out += "offsets=";
    for (std::size_t i = 0; i < s.offsets.size(); ++i)
        out += (i ? "," : "") + num(s.offsets[i]);
    out += "\n";
    if (!s.colors.empty()) {
        out += "colors=";
        for (std::size_t i = 0; i < s.colors.size(); ++i)
            out += (i ? "," : "") + std::to_string(s.colors[i]);
        out += "\n";
    }
    if (!s.linetypes.empty()) {
        out += "linetypes=";
        for (std::size_t i = 0; i < s.linetypes.size(); ++i)
            out += (i ? "," : "") + s.linetypes[i];
        out += "\n";
    }
    if (s.fill >= 0)
        out += "fill=" + std::to_string(s.fill) + "\n";
    if (s.joints)
        out += "joints=1\n";
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
        } else if (key == "arcs" && val.size() >= 2) {
            s.startArc = std::clamp(val[0] - '0', 0, 2);
            s.endArc = std::clamp(val[1] - '0', 0, 2);
        } else if (key == "colors") {
            std::istringstream in(val);
            std::string item;
            while (std::getline(in, item, ','))
                s.colors.push_back(std::clamp(std::atoi(item.c_str()), 0, 256));
        } else if (key == "linetypes") {
            std::string item;
            std::size_t from = 0;
            while (true) {
                const auto comma = val.find(',', from);
                s.linetypes.push_back(val.substr(
                    from, comma == std::string::npos ? std::string::npos : comma - from));
                if (comma == std::string::npos)
                    break;
                from = comma + 1;
            }
        } else if (key == "fill") {
            s.fill = std::clamp(std::atoi(val.c_str()), -1, 256);
        } else if (key == "joints") {
            s.joints = val == "1";
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
    if (s.colors.size() > s.offsets.size())
        s.colors.resize(s.offsets.size());
    if (s.linetypes.size() > s.offsets.size())
        s.linetypes.resize(s.offsets.size());
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
    std::vector<std::size_t> kept(d.v.size());  // vertex of the source path -> vertex of `p`
    for (std::size_t j = 0; j < d.v.size(); ++j) {
        if (p.empty() || distance(p.back(), d.v[j]) > 1e-12)
            p.push_back(d.v[j]);
        kept[j] = p.size() - 1;
    }
    bool closed = d.closed;
    if (closed && p.size() > 2 && distance(p.front(), p.back()) <= 1e-12) {
        for (std::size_t& k : kept)
            if (k == p.size() - 1)
                k = 0;
        p.pop_back();
    }
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
    // Rank of each style line from the lowest offset; cuts name lines by rank.
    std::vector<std::size_t> order(style.offsets.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return style.offsets[a] < style.offsets[b]; });
    std::vector<std::size_t> rank(order.size());
    for (std::size_t r = 0; r < order.size(); ++r)
        rank[order[r]] = r;
    std::vector<std::vector<Vec2>> capPoints(style.offsets.size());
    for (std::size_t k = 0; k < style.offsets.size(); ++k) {
        const double o = (style.offsets[k] + shift) * d.scale;
        std::vector<Vec2> line;
        line.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            line.push_back(p[i] + miter[i] * o);
        capPoints[k] = line;
        std::vector<std::pair<double, double>> gaps;  // in segment + fraction units
        for (const MlineCut& c : d.cuts) {
            if (rank[k] < c.lo || rank[k] > c.hi || c.seg >= d.v.size())
                continue;
            // The cut names a segment of the path as drawn, repeated points included.
            const std::size_t from = kept[c.seg];
            const std::size_t to = kept[(c.seg + 1) % d.v.size()];
            if (from == to || from >= segs || (!d.closed && c.seg + 1 >= d.v.size()))
                continue;
            const double a = std::clamp(c.t0, 0.0, 1.0), b = std::clamp(c.t1, 0.0, 1.0);
            gaps.emplace_back(from + std::min(a, b), from + std::max(a, b));
        }
        if (gaps.empty()) {
            out.lines.push_back(std::move(line));
            out.closed.push_back(closed);
            out.element.push_back(static_cast<int>(k));
            continue;
        }
        if (closed)
            line.push_back(line.front());  // cut a closed line open at its first vertex
        std::sort(gaps.begin(), gaps.end());
        auto at = [&](double u) {
            const std::size_t i = std::min(static_cast<std::size_t>(u), line.size() - 2);
            const double t = u - static_cast<double>(i);
            return line[i] + (line[i + 1] - line[i]) * t;
        };
        const double end = static_cast<double>(line.size() - 1);
        double from = 0;
        auto emit = [&](double a, double b) {
            if (b - a < 1e-9)
                return;
            std::vector<Vec2> piece{at(a)};
            for (std::size_t i = static_cast<std::size_t>(std::floor(a)) + 1;
                 static_cast<double>(i) < b; ++i)
                piece.push_back(line[i]);
            piece.push_back(at(b));
            out.lines.push_back(std::move(piece));
            out.closed.push_back(false);
            out.element.push_back(static_cast<int>(k));
        };
        for (const auto& [a, b] : gaps) {
            emit(from, std::max(from, a));
            from = std::max(from, b);
        }
        emit(from, end);
    }
    if (!closed && out.lines.size() >= 2) {
        // A cap runs across the end, from the lowest offset to the highest.
        auto cap = [&](std::size_t idx) {
            std::vector<std::pair<double, Vec2>> ends;
            for (std::size_t i = 0; i < style.offsets.size(); ++i)
                ends.emplace_back(style.offsets[i], capPoints[i][idx]);
            std::sort(ends.begin(), ends.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            out.lines.push_back({ends.front().second, ends.back().second});
            out.closed.push_back(false);
            out.element.push_back(-1);
        };
        if (style.startCap)
            cap(0);
        if (style.endCap)
            cap(n - 1);
        // Arcs across an end: outer = one half circle over all the lines, inner = half circles
        // joining the lines in pairs from the outside in (the outermost pair is left out).
        auto arcs = [&](std::size_t idx, int kind, bool atStart) {
            if (kind == 0)
                return;
            const Vec2 dir = normalized(atStart ? p[0] - p[1] : p[n - 1] - p[n - 2]);  // outwards
            std::vector<std::pair<double, Vec2>> ends;
            for (std::size_t i = 0; i < style.offsets.size(); ++i)
                ends.emplace_back(style.offsets[i], capPoints[i][idx]);
            std::sort(ends.begin(), ends.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            const std::size_t count = ends.size();
            // outer: the outermost pair; inner: the pairs inside it, outermost first
            for (std::size_t lo = kind == 1 ? 0 : 1; lo < count - 1 - lo; ++lo) {
                const std::size_t hi = count - 1 - lo;
                const Vec2 a = ends[lo].second, b = ends[hi].second;
                const Vec2 mid = (a + b) * 0.5;
                const Vec2 half = (b - a) * 0.5;
                std::vector<Vec2> arc;
                constexpr int kSteps = 24;
                // from a through the outward point to b
                for (int k = 0; k <= kSteps; ++k) {
                    const double t = std::acos(-1.0) * k / kSteps;
                    // half rotated by t about the centre, bulging along `dir`
                    const double c = std::cos(t), sn = std::sin(t);
                    const Vec2 across = half * -c;
                    const double r = std::hypot(half.x, half.y);
                    arc.push_back(mid + across + dir * (r * sn));
                }
                out.lines.push_back(std::move(arc));
                out.closed.push_back(false);
                out.element.push_back(-1);
                if (kind == 1)
                    break;
            }
        };
        arcs(0, style.startArc, true);
        arcs(n - 1, style.endArc, false);
    }
    // The outer lines of the style (lowest and highest offset) bound its fill and its joints.
    if (style.offsets.size() >= 2) {
        const std::size_t low = order.front(), high = order.back();
        if (style.joints) {
            for (std::size_t i = closed ? 0 : 1; i + (closed ? 0 : 1) < n; ++i) {
                out.lines.push_back({capPoints[low][i], capPoints[high][i]});
                out.closed.push_back(false);
                out.element.push_back(-1);
            }
        }
        if (style.fill >= 0) {
            if (closed) {
                out.fill.push_back(capPoints[low]);
                out.fill.push_back(capPoints[high]);
            } else {
                std::vector<Vec2> ring = capPoints[low];
                ring.insert(ring.end(), capPoints[high].rbegin(), capPoints[high].rend());
                out.fill.push_back(std::move(ring));
            }
        }
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
        if (const auto style = findStyle(doc, d->style); style && s.element[i] >= 0) {
            const std::size_t k = static_cast<std::size_t>(s.element[i]);
            const int c = style->colorOf(k);
            if (c == 0)
                n.common.color = Color::byBlock();
            else if (c < 256)
                n.common.color = Color::index(static_cast<std::uint8_t>(c));
            if (!style->linetypeOf(k).empty())
                if (const Linetype* lt = doc.linetypeByName(style->linetypeOf(k)))
                    n.common.linetype = lt->id;
        }
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
    if (!d.cuts.empty()) {
        out += "cuts=";
        for (std::size_t i = 0; i < d.cuts.size(); ++i) {
            const MlineCut& c = d.cuts[i];
            out += (i ? ";" : "") + std::to_string(c.seg) + "," + num(c.t0) + "," + num(c.t1) +
                   "," + std::to_string(c.lo) + "," + std::to_string(c.hi);
        }
        out += "\n";
    }
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
            } else if (key == "cuts") {
                std::istringstream in(val);
                std::string item;
                while (std::getline(in, item, ';')) {
                    std::istringstream f(item);
                    std::string part;
                    std::vector<std::string> parts;
                    while (std::getline(f, part, ','))
                        parts.push_back(part);
                    if (parts.size() != 5)
                        return std::nullopt;
                    MlineCut c;
                    c.seg = static_cast<std::uint32_t>(std::stoul(parts[0]));
                    c.t0 = std::stod(parts[1]);
                    c.t1 = std::stod(parts[2]);
                    c.lo = static_cast<std::uint8_t>(std::stoi(parts[3]));
                    c.hi = static_cast<std::uint8_t>(std::stoi(parts[4]));
                    p.data.cuts.push_back(c);
                }
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
