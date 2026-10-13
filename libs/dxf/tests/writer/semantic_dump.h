// Test helper: a dump of a Document that does not depend on the numeric ids the importer
// happened to assign (entity, layer, block, style ids) and compares numbers with a tolerance.
// Two documents that look the same to a user have equal semantic dumps. Used by the DXF round
// trip tests (DXF -> document -> DXF -> document) in libs/dxf and apps/tbcad-convert.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/core/dump.h"

namespace tbcad::dxf::testing {

class SemanticDump {
public:
    explicit SemanticDump(const Document& doc) : doc_(doc) { build(); }

    const std::vector<std::string>& lines() const { return lines_; }

    // Empty when equal, else a description of the first difference. Numbers match when they are
    // within `relTol` (relative to their size, at least `absTol` apart in absolute terms).
    static std::string diff(const SemanticDump& a, const SemanticDump& b, double relTol = 1e-9,
                            double absTol = 1e-9) {
        if (a.lines_.size() != b.lines_.size()) {
            return "line count " + std::to_string(a.lines_.size()) + " vs " +
                   std::to_string(b.lines_.size()) + firstMissing(a, b);
        }
        for (std::size_t i = 0; i < a.lines_.size(); ++i) {
            if (!sameLine(a.lines_[i], b.lines_[i], relTol, absTol))
                return "line " + std::to_string(i) + ":\n  " + a.lines_[i] + "\n  " + b.lines_[i];
        }
        return {};
    }

private:
    const Document& doc_;
    std::vector<std::string> lines_{};

    static std::string firstMissing(const SemanticDump& a, const SemanticDump& b) {
        std::size_t n = std::min(a.lines_.size(), b.lines_.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (!sameLine(a.lines_[i], b.lines_[i], 1e-9, 1e-9))
                return "; first difference at line " + std::to_string(i) + ":\n  " + a.lines_[i] +
                       "\n  " + b.lines_[i];
        }
        const auto& longer = a.lines_.size() > b.lines_.size() ? a.lines_ : b.lines_;
        return "; extra line:\n  " + longer[n];
    }

    static bool sameLine(const std::string& x, const std::string& y, double relTol, double absTol) {
        static const std::regex number(R"(-?\d+(\.\d+)?([eE][-+]?\d+)?)");
        auto split = [&](const std::string& s, std::string& skeleton, std::vector<double>& nums) {
            auto begin = std::sregex_iterator(s.begin(), s.end(), number);
            std::size_t last = 0;
            for (auto it = begin; it != std::sregex_iterator(); ++it) {
                skeleton.append(s, last, static_cast<std::size_t>(it->position()) - last);
                skeleton += '#';
                nums.push_back(std::strtod(it->str().c_str(), nullptr));
                last = static_cast<std::size_t>(it->position() + it->length());
            }
            skeleton.append(s, last, std::string::npos);
        };
        std::string sx, sy;
        std::vector<double> nx, ny;
        split(x, sx, nx);
        split(y, sy, ny);
        if (sx != sy || nx.size() != ny.size())
            return false;
        for (std::size_t i = 0; i < nx.size(); ++i) {
            double scale = std::max(std::abs(nx[i]), std::abs(ny[i]));
            if (std::abs(nx[i] - ny[i]) > std::max(absTol, relTol * scale))
                return false;
        }
        return true;
    }

    std::string layerPath(LayerId id) const {
        std::string path;
        for (LayerId p = id; p;) {
            const Layer* l = doc_.layer(p);
            if (!l)
                return "?" + std::to_string(id.value);
            path = path.empty() ? l->name : l->name + "/" + path;
            p = l->parent;
        }
        return path;
    }
    std::string ltName(LinetypeId id) const {
        if (id == kLinetypeByLayer)
            return "ByLayer";
        if (id == kLinetypeByBlock)
            return "ByBlock";
        if (id == kLinetypeByParent)
            return "ByParent";
        const Linetype* lt = doc_.linetype(id);
        return lt ? lt->name : "?lt";
    }
    // An entity without a style uses the document's current one.
    std::string tsName(TextStyleId id) const {
        if (!id)
            id = doc_.header().currentTextStyle;
        const TextStyle* s = doc_.textStyle(id);
        return s ? s->name : (id ? "?ts" : "");
    }
    std::string dsName(DimStyleId id) const {
        if (!id)
            id = doc_.header().currentDimStyle;
        const DimStyle* s = doc_.dimStyle(id);
        return s ? s->name : (id ? "?ds" : "");
    }
    std::string blockName(BlockId id) const {
        const BlockDef* b = doc_.block(id);
        return b ? b->name : "?block";
    }

    // Rewrites the id-bearing tokens of dumpEntity() output (outside quoted strings).
    std::string entityLine(const Entity& e) const {
        std::string raw = dumpEntity(e);
        std::string out;
        std::string plain;
        auto flush = [&] {
            plain = rewrite(plain, e);
            out += plain;
            plain.clear();
        };
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '"') {
                flush();
                std::size_t j = i + 1;
                while (j < raw.size() && raw[j] != '"') {
                    if (raw[j] == '\\')
                        ++j;
                    ++j;
                }
                out.append(raw, i, j - i + 1);
                i = j;
            } else {
                plain += raw[i];
            }
        }
        flush();
        return dropRingClosers(out);
    }

    // "ring (a) (b) (c) (a);" and "ring (a) (b) (c);" draw the same filled area: the importer
    // repeats the first point for closed boundaries and the exporter does not need to.
    static std::string dropRingClosers(const std::string& s) {
        std::string out;
        std::size_t pos = 0;
        while (true) {
            std::size_t ring = s.find("ring ", pos);
            if (ring == std::string::npos) {
                out.append(s, pos, std::string::npos);
                return out;
            }
            std::size_t start = ring + 5;
            std::size_t end = s.find(';', start);
            if (end == std::string::npos) {
                out.append(s, pos, std::string::npos);
                return out;
            }
            std::vector<std::string> pts;
            std::size_t i = start;
            while (i < end) {
                std::size_t close = s.find(')', i);
                pts.push_back(s.substr(i, close - i + 1));
                i = close + 2;  // skip the space
            }
            if (pts.size() > 3 && pts.front() == pts.back())
                pts.pop_back();
            out.append(s, pos, start - pos);
            for (std::size_t k = 0; k < pts.size(); ++k)
                out += (k ? " " : "") + pts[k];
            pos = end;
        }
    }

    std::string rewrite(std::string s, const Entity& e) const {
        static const std::regex entityId(R"(\be\d+\b)");
        static const std::regex layerTok(R"(\bL(\d+)\b)");
        static const std::regex ltTok(R"(\blt(\d+)\b)");
        static const std::regex styleTok(R"(\bstyle(\d+)\b)");
        static const std::regex blockTok(R"(\bblock(\d+)\b)");
        static const std::regex ownerTok(R"(\bowner\d+\b)");
        s = std::regex_replace(s, entityId, "e");
        s = std::regex_replace(s, ownerTok, "owner");
        s = replaceWith(s, layerTok, [&](unsigned long id) {
            return "L=" + layerPath(LayerId{static_cast<std::uint32_t>(id)});
        });
        s = replaceWith(s, ltTok, [&](unsigned long id) {
            return "lt=" + ltName(LinetypeId{static_cast<std::uint32_t>(id)});
        });
        EntityKind k = kind(e);
        bool dimLike = k == EntityKind::Dimension || k == EntityKind::Leader;
        s = replaceWith(s, styleTok, [&](unsigned long id) {
            return "style=" + (dimLike ? dsName(DimStyleId{static_cast<std::uint32_t>(id)})
                                       : tsName(TextStyleId{static_cast<std::uint32_t>(id)}));
        });
        // Rotation angles are the same angle modulo a full turn (a vector read back with atan2
        // lands in (-pi, pi], a stored angle in [0, 2pi)).
        {
            static const std::regex rotTok(R"(\brot(-?\d+(\.\d+)?([eE][-+]?\d+)?))");
            std::string r;
            std::size_t last = 0;
            for (auto it = std::sregex_iterator(s.begin(), s.end(), rotTok);
                 it != std::sregex_iterator(); ++it) {
                r.append(s, last, static_cast<std::size_t>(it->position()) - last);
                double a = normalizeAngle(std::strtod((*it)[1].str().c_str(), nullptr));
                if (kTwoPi - a < 1e-9)
                    a = 0;
                r += "rot" + formatDouble(a);
                last = static_cast<std::size_t>(it->position() + it->length());
            }
            r.append(s, last, std::string::npos);
            s = r;
        }
        s = replaceWith(s, blockTok, [&](unsigned long id) {
            return "block=" + blockName(BlockId{static_cast<std::uint32_t>(id)});
        });
        return s;
    }

    template <class Fn>
    static std::string replaceWith(const std::string& s, const std::regex& re, Fn fn) {
        std::string out;
        std::size_t last = 0;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator();
             ++it) {
            out.append(s, last, static_cast<std::size_t>(it->position()) - last);
            out += fn(std::strtoul((*it)[1].str().c_str(), nullptr, 10));
            last = static_cast<std::size_t>(it->position() + it->length());
        }
        out.append(s, last, std::string::npos);
        return out;
    }

    static bool isDimensionBlock(const BlockDef& b) {
        return b.name.size() >= 2 && b.name[0] == '*' && (b.name[1] == 'D' || b.name[1] == 'd');
    }

    void build() {
        const Header& h = doc_.header();
        lines_.push_back("header ltscale " + formatDouble(h.ltScale) + " units " +
                         std::to_string(static_cast<int>(h.units)));

        std::vector<std::string> rows;
        for (const auto& [id, l] : doc_.layers()) {
            std::string r = "layer " + layerPath(id) + " color " +
                            std::to_string(int(l.color.kind)) + ":" + std::to_string(l.color.r) +
                            "." + std::to_string(l.color.g) + "." + std::to_string(l.color.b) +
                            " lt " + ltName(l.linetype) + " lw " + std::to_string(l.lineweight.v) +
                            " on" + std::to_string(l.on) + " frozen" + std::to_string(l.frozen) +
                            " locked" + std::to_string(l.locked) + " plot" + std::to_string(l.plot);
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end());
        lines_.insert(lines_.end(), rows.begin(), rows.end());

        rows.clear();
        for (const auto& [id, lt] : doc_.linetypes()) {
            std::string r = "linetype " + lt.name + " pattern";
            for (double d : lt.pattern)
                r += " " + formatDouble(d);
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end());
        lines_.insert(lines_.end(), rows.begin(), rows.end());

        rows.clear();
        for (const auto& [id, s] : doc_.textStyles()) {
            // An empty font is written as "txt", AutoCAD's default.
            rows.push_back("textstyle " + s.name + " font " +
                           (s.fontFile.empty() ? std::string("txt") : s.fontFile) + " wf " +
                           formatDouble(s.widthFactor) + " ob " + formatDouble(s.oblique));
        }
        std::sort(rows.begin(), rows.end());
        lines_.insert(lines_.end(), rows.begin(), rows.end());

        rows.clear();
        for (const auto& [id, s] : doc_.dimStyles()) {
            rows.push_back("dimstyle " + s.name + " th " + formatDouble(s.textHeight) + " gap " +
                           formatDouble(s.textGap) + " asz " + formatDouble(s.arrowSize) + " exo " +
                           formatDouble(s.extLineOffset) + " exe " +
                           formatDouble(s.extLineExtension) + " dle " +
                           formatDouble(s.dimLineExtension) + " scale " +
                           formatDouble(s.overallScale) + " prec " + std::to_string(s.precision));
        }
        std::sort(rows.begin(), rows.end());
        lines_.insert(lines_.end(), rows.begin(), rows.end());

        // Blocks by name; dimension blocks are regenerated by the exporter and left out.
        std::vector<const BlockDef*> blocks;
        for (const auto& [id, b] : doc_.blocks()) {
            if (!(b.anonymous && isDimensionBlock(b)))
                blocks.push_back(&b);
        }
        std::sort(blocks.begin(), blocks.end(),
                  [](const BlockDef* x, const BlockDef* y) { return x->name < y->name; });
        for (const BlockDef* b : blocks) {
            lines_.push_back("block " + b->name + " base (" + formatDouble(b->base.x) + "," +
                             formatDouble(b->base.y) + ") anon " + std::to_string(b->anonymous));
            for (EntityId id : b->entities) {
                if (const Entity* e = doc_.find(id))
                    lines_.push_back("  " + entityLine(*e));
            }
        }
        for (EntityId id : doc_.modelSpace()) {
            if (const Entity* e = doc_.find(id))
                lines_.push_back(entityLine(*e));
        }
    }
};

}  // namespace tbcad::dxf::testing
