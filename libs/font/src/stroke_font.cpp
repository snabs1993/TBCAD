#include "stroke_font.h"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace tbcad::font {

namespace {

bool parseNumber(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(out);
}

}  // namespace

std::string StrokeFont::parse(std::string_view text) {
    glyphs_.clear();
    family_.clear();
    upm_ = 20;
    toneDrop_ = 0;
    double cap = 14, xh = 10, asc = 18, desc = -5, drop = 0;
    bool header = false;
    Glyph* cur = nullptr;
    int lineNo = 0;

    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        if (auto hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;)
            tok.push_back(std::move(t));
        if (tok.empty())
            continue;
        auto fail = [&](const char* why) { return "line " + std::to_string(lineNo) + ": " + why; };
        const std::string& key = tok[0];
        if (key == "tbsf") {
            header = true;
            if (tok.size() < 2 || tok[1] != "1")
                return fail("unsupported tbsf version");
        } else if (!header) {
            return fail("not a tbsf file (first line must be 'tbsf 1')");
        } else if (key == "family") {
            family_ = line.substr(line.find("family") + 6);
            family_.erase(0, family_.find_first_not_of(" \t"));
            family_.erase(family_.find_last_not_of(" \t\r") + 1);
        } else if (key == "upm" || key == "cap" || key == "xheight" || key == "ascender" ||
                   key == "descender" || key == "tonedrop") {
            double v = 0;
            if (tok.size() < 2 || !parseNumber(tok[1], v))
                return fail("bad number");
            if (key == "upm") {
                if (v < 4 || v > 4096)
                    return fail("upm out of range");
                upm_ = static_cast<int>(v);
            } else if (key == "cap") {
                cap = v;
            } else if (key == "xheight") {
                xh = v;
            } else if (key == "ascender") {
                asc = v;
            } else if (key == "descender") {
                desc = v;
            } else {
                drop = v;
            }
        } else if (key == "glyph") {
            double adv = 0;
            if (tok.size() < 3 || !parseNumber(tok[2], adv) || adv < 0)
                return fail("glyph needs a hex code point and an advance");
            char* end = nullptr;
            unsigned long cp = std::strtoul(tok[1].c_str(), &end, 16);
            if (*end != '\0' || cp == 0 || cp > 0x10FFFF)
                return fail("bad code point");
            Glyph g;
            g.advance = adv;  // design units for now, scaled below
            for (std::size_t i = 3; i < tok.size(); ++i) {
                if (tok[i] == "mark=upper")
                    g.mark = Mark::Upper;
                else if (tok[i] == "mark=tone")
                    g.mark = Mark::Tone;
                else if (tok[i] == "mark=lower")
                    g.mark = Mark::Lower;
            }
            cur = &(glyphs_[static_cast<char32_t>(cp)] = std::move(g));
        } else if (key == "s") {
            if (!cur)
                return fail("stroke before any glyph");
            if (tok.size() < 5 || (tok.size() - 1) % 2 != 0)
                return fail("a stroke needs at least two x y pairs");
            StrokePath p;
            for (std::size_t i = 1; i + 1 < tok.size(); i += 2) {
                double x = 0, y = 0;
                if (!parseNumber(tok[i], x) || !parseNumber(tok[i + 1], y))
                    return fail("bad coordinate");
                p.points.push_back({x, y});  // design units for now
            }
            cur->strokes.paths.push_back(std::move(p));
        }
        // Unknown keys are ignored so newer files stay readable.
    }
    if (!header)
        return "not a tbsf file";
    if (glyphs_.empty())
        return "font has no glyphs";

    const double u = 1.0 / upm_;
    for (auto& [cp, g] : glyphs_) {
        g.advance *= u;
        for (auto& p : g.strokes.paths)
            for (auto& pt : p.points) {
                pt.x *= u;
                pt.y *= u;
            }
    }
    metrics_.unitsPerEm = upm_;
    metrics_.capHeight = cap * u;
    metrics_.xHeight = xh * u;
    metrics_.ascender = asc * u;
    metrics_.descender = desc * u;
    toneDrop_ = drop * u;
    if (family_.empty())
        family_ = "TBCAD Stroke";
    return {};
}

}  // namespace tbcad::font
