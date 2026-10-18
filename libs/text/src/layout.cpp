#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "tbcad/text/field.h"
#include "text_internal.h"

namespace tbcad::text {

double syntheticItalicShear() {
    return std::tan(12.0 * kPi / 180.0);
}

namespace {

constexpr char32_t kZwsp = 0x200B;

// ---- shaping -------------------------------------------------------------------------------

struct Chunk {
    std::vector<PlacedGlyph> glyphs{};  // origin.x relative to the chunk start, origin.y = baseline
    double advance = 0;
};

struct FontInfo {
    font::FontId id{};
    double capHeight = 0.7;
    double descender = -0.2;  // em, negative below the baseline
    double shear = 0;
};

FontInfo fontInfo(const TextStyle* style, const Attr& attr) {
    FontInfo fi;
    fi.id = fontForStyle(style, attr.bold, attr.italic);
    if (fi.id != kNoFont) {
        font::FontMetrics m = fonts().metrics(fi.id);
        if (m.capHeight > 0.05)
            fi.capHeight = m.capHeight;
        fi.descender = m.descender < 0 ? m.descender : -0.2;
    }
    if (attr.italic && fontForStyle(style, false, false) == fi.id)
        fi.shear = syntheticItalicShear();
    return fi;
}

std::string stripInvisible(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = 0;
        std::size_t len = detail::decodeAt(s, i, cp);
        if (cp != kZwsp && cp != '\n' && cp != '\r')
            out.append(s.substr(i, len));
        i += len;
    }
    return out;
}

Chunk shapeChunk(const TextStyle* style, std::string_view text, const Attr& attr) {
    Chunk c;
    std::string clean = stripInvisible(text);
    if (clean.empty() || !(attr.height > 0))
        return c;
    FontInfo fi = fontInfo(style, attr);
    if (fi.id == kNoFont)
        return c;
    const double s = attr.height / fi.capHeight;  // mm per em
    double pen = 0;
    for (const font::ShapedRun& run : fonts().shape(fi.id, clean)) {
        const double k = s / std::max(1, run.unitsPerEm);
        for (const font::ShapedGlyph& g : run.glyphs) {
            PlacedGlyph pg;
            pg.font = run.font;
            pg.glyphId = g.glyphId;
            pg.origin = {pen + g.xOffset * k, g.yOffset * k};
            pg.scale = s;  // mm per em
            pg.shear = fi.shear;
            pg.color = attr.color;
            c.glyphs.push_back(pg);
            pen += g.xAdvance * k;
        }
    }
    c.advance = pen;
    return c;
}

// ---- glyph boxes -------------------------------------------------------------------------------

BBox2 glyphBoxEm(font::FontId f, std::uint32_t gid) {
    static std::mutex m;
    static std::unordered_map<std::uint64_t, BBox2> cache;
    std::uint64_t key = (static_cast<std::uint64_t>(f.value) << 32) | gid;
    {
        std::lock_guard lock(m);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    BBox2 b;
    if (fonts().isStroke(f)) {
        for (const auto& path : fonts().strokes(f, gid).paths)
            for (const auto& p : path.points)
                b.expand({p.x, p.y});
    } else {
        const font::GlyphOutline& o = fonts().outline(f, gid);
        for (const auto& c : o.contours)
            for (const auto& p : c.points)
                b.expand({p.x, p.y});
    }
    std::lock_guard lock(m);
    cache.emplace(key, b);
    return b;
}

BBox2 glyphBoxLayout(const PlacedGlyph& g) {
    BBox2 e = glyphBoxEm(g.font, g.glyphId);
    BBox2 out;
    if (e.empty())
        return out;
    for (Vec2 c : {Vec2{e.min.x, e.min.y}, Vec2{e.max.x, e.min.y}, Vec2{e.min.x, e.max.y},
                   Vec2{e.max.x, e.max.y}})
        out.expand({g.origin.x + (c.x + g.shear * c.y) * g.scale, g.origin.y + c.y * g.scale});
    return out;
}

// ---- line building
// -------------------------------------------------------------------------------

// A stacked fraction is smaller than the text around it; the bar sits at mid height.
constexpr double kStackScale = 0.7;
constexpr double kStackGap = 0.1;  // between bar and digits, in text heights

struct Stack {
    std::string num{}, den{};
    char bar = '/';
};

struct Item {
    std::string text{};
    Attr attr{};
    bool space = false;
    double width = 0;
    std::optional<Stack> stack{};
};

using Line = std::vector<Item>;

struct Token {
    std::string text{};
    Attr attr{};
    enum class Kind { Word, Space, Zwsp } kind = Kind::Word;
    std::optional<Stack> stack{};
};

struct Unit {
    std::vector<Token> word{};
    std::optional<Token> brk{};
};

double itemWidth(const TextStyle* style, const std::string& text, const Attr& attr) {
    return shapeChunk(style, text, attr).advance;
}

Attr stackAttr(const Attr& attr) {
    Attr a = attr;
    a.height = attr.height * kStackScale;
    a.underline = a.overline = false;
    return a;
}

double stackWidth(const TextStyle* style, const Stack& st, const Attr& attr) {
    const Attr a = stackAttr(attr);
    return std::max(itemWidth(style, st.num, a), itemWidth(style, st.den, a));
}

std::vector<Unit> splitUnits(const std::vector<Piece>& pieces) {
    std::vector<Unit> units;
    Unit cur;
    auto flushWordText = [&](std::string& buf, const Attr& attr) {
        if (buf.empty())
            return;
        // Thai has no spaces: the word list gives soft break opportunities between words.
        std::size_t from = 0;
        for (std::size_t at : thaiWordBreaks(buf)) {
            cur.word.push_back({buf.substr(from, at - from), attr, Token::Kind::Word});
            Token t;
            t.attr = attr;
            t.kind = Token::Kind::Zwsp;
            cur.brk = std::move(t);
            units.push_back(std::move(cur));
            cur = Unit{};
            from = at;
        }
        cur.word.push_back({buf.substr(from), attr, Token::Kind::Word});
        buf.clear();
    };
    for (const Piece& p : pieces) {
        if (p.stacked) {
            Token t;
            t.text = p.text;
            t.attr = p.attr;
            t.kind = Token::Kind::Word;
            t.stack = Stack{p.num, p.den, p.bar};
            cur.word.push_back(std::move(t));
            continue;
        }
        std::string buf;
        for (std::size_t i = 0; i < p.text.size();) {
            char32_t cp = 0;
            std::size_t len = detail::decodeAt(p.text, i, cp);
            if (cp == ' ' || cp == '\t' || cp == kZwsp) {
                flushWordText(buf, p.attr);
                Token t;
                t.attr = p.attr;
                t.kind = cp == kZwsp ? Token::Kind::Zwsp : Token::Kind::Space;
                t.text = cp == kZwsp ? std::string{} : std::string(" ");
                cur.brk = std::move(t);
                units.push_back(std::move(cur));
                cur = Unit{};
            } else {
                buf.append(p.text, i, len);
            }
            i += len;
        }
        flushWordText(buf, p.attr);
    }
    if (!cur.word.empty() || cur.brk)
        units.push_back(std::move(cur));
    return units;
}

class LineBuilder {
public:
    LineBuilder(const TextStyle* style, double wrapWidth) : style_(style), wrap_(wrapWidth) {}

    void addParagraph(const std::vector<Piece>& pieces) {
        cur_.clear();
        std::vector<Unit> units = splitUnits(pieces);
        for (const Unit& u : units) {
            double wg = 0;
            std::vector<Item> items;
            for (const Token& t : u.word) {
                Item it{t.text, t.attr, false,
                        t.stack ? stackWidth(style_, *t.stack, t.attr)
                                : itemWidth(style_, t.text, t.attr),
                        t.stack};
                wg += it.width;
                items.push_back(std::move(it));
            }
            if (wrap_ > 0 && !items.empty()) {
                if (wg <= wrap_ + kEps) {
                    if (!cur_.empty() && usedWidth() + wg > wrap_ + kEps)
                        newLine();
                    for (Item& it : items)
                        push(std::move(it));
                } else {
                    // Wider than a whole line: break before it, then fill by clusters.
                    if (!cur_.empty())
                        newLine();
                    splitByClusters(u);
                }
            } else {
                for (Item& it : items)
                    push(std::move(it));
            }
            if (u.brk && u.brk->kind == Token::Kind::Space) {
                Item sp{u.brk->text, u.brk->attr, true,
                        itemWidth(style_, u.brk->text, u.brk->attr)};
                push(std::move(sp));
            }
        }
        // End of paragraph.
        lines_.push_back(std::move(cur_));
        cur_.clear();
    }

    void addEmptyParagraph(const Attr& attr) {
        // A blank line keeps the height of the paragraph break that ended it.
        Line l;
        l.push_back({std::string{}, attr, true, 0});
        lines_.push_back(std::move(l));
    }

    std::vector<Line>& lines() { return lines_; }

private:
    static constexpr double kEps = 1e-9;

    double totalWidth() const {
        double w = 0;
        for (const Item& it : cur_)
            w += it.width;
        return w;
    }
    // Width the line occupies if more is appended after its trailing spaces.
    double usedWidth() const { return totalWidth(); }

    void push(Item it) { cur_.push_back(std::move(it)); }

    void newLine() {
        while (!cur_.empty() && cur_.back().space)
            cur_.pop_back();  // spaces at a wrap point are dropped
        lines_.push_back(std::move(cur_));
        cur_.clear();
    }

    // The unit is wider than the wrap width: fill the line cluster by cluster. A cluster is
    // never split.
    void splitByClusters(const Unit& u) {
        for (const Token& t : u.word) {
            if (t.stack) {  // a fraction is never split
                const double w = stackWidth(style_, *t.stack, t.attr);
                if (!cur_.empty() && totalWidth() + w > wrap_ + kEps)
                    newLine();
                push({t.text, t.attr, false, w, t.stack});
                continue;
            }
            auto bounds = clusterBoundaries(t.text);
            for (std::size_t k = 0; k + 1 < bounds.size(); ++k) {
                std::string cl = t.text.substr(bounds[k], bounds[k + 1] - bounds[k]);
                double w = itemWidth(style_, cl, t.attr);
                if (!cur_.empty() && totalWidth() + w > wrap_ + kEps)
                    newLine();
                push({std::move(cl), t.attr, false, w});
            }
        }
    }

    const TextStyle* style_;
    double wrap_;
    Line cur_{};
    std::vector<Line> lines_{};
};

// Emits glyphs for a finished line at baseline y; returns its width (trailing spaces excluded).
LayoutLine emitLine(const TextStyle* style, const Line& line, double baseline,
                    std::vector<PlacedGlyph>& glyphs, double defaultHeight) {
    LayoutLine out;
    out.baseline = baseline;
    out.firstGlyph = glyphs.size();
    out.height = 0;
    // Drop trailing spaces.
    std::size_t n = line.size();
    while (n > 0 && line[n - 1].space)
        --n;
    double pen = 0;
    std::size_t i = 0;
    while (i < n) {
        if (line[i].stack) {
            const Item& it = line[i];
            const Stack& st = *it.stack;
            const Attr sa = stackAttr(it.attr);
            const double H = it.attr.height;
            const double mid = baseline + 0.5 * H;
            const Chunk num = shapeChunk(style, st.num, sa);
            const Chunk den = shapeChunk(style, st.den, sa);
            const double w = it.width;
            // Numerator above the bar, denominator below it, both centred.
            auto put = [&](const Chunk& c, double x, double y) {
                for (PlacedGlyph g : c.glyphs) {
                    g.origin.x += x;
                    g.origin.y += y;
                    glyphs.push_back(g);
                }
            };
            if (st.bar == '#') {
                // Diagonal bar: numerator high-left, denominator low-right.
                put(num, pen, baseline + 0.5 * H + kStackGap * H);
                put(den, pen + w - den.advance, baseline - 0.3 * H);
                out.rules.push_back({pen, pen + w, mid});  // drawn level; no slanted rules exist
            } else {
                put(num, pen + (w - num.advance) / 2, mid + kStackGap * H);
                put(den, pen + (w - den.advance) / 2, mid - kStackGap * H - sa.height);
                if (st.bar == '/')
                    out.rules.push_back({pen, pen + w, mid});
            }
            out.height = std::max(out.height, H);
            out.text += st.num + "/" + st.den;
            pen += w;
            ++i;
            continue;
        }
        // Merge consecutive items with equal attributes into one shaped chunk.
        std::string text = line[i].text;
        Attr attr = line[i].attr;
        std::size_t j = i + 1;
        while (j < n && !line[j].stack && line[j].attr == attr) {
            text += line[j].text;
            ++j;
        }
        out.height = std::max(out.height, attr.height);
        out.text += text;
        Chunk c = shapeChunk(style, text, attr);
        for (PlacedGlyph g : c.glyphs) {
            g.origin.x += pen;
            g.origin.y += baseline;
            glyphs.push_back(g);
        }
        // Rules end where the chunk's text does; a chunk that is only spaces still carries one.
        if (attr.underline)
            out.rules.push_back({pen, pen + c.advance, baseline - 0.18 * attr.height});
        if (attr.overline)
            out.rules.push_back({pen, pen + c.advance, baseline + 1.18 * attr.height});
        pen += c.advance;
        i = j;
    }
    if (out.height <= 0) {
        for (const Item& it : line)
            out.height = std::max(out.height, it.attr.height);
        if (out.height <= 0)
            out.height = defaultHeight;
    }
    out.width = pen;
    out.endGlyph = glyphs.size();
    return out;
}

void shiftLayout(TextLayout& l, Vec2 d) {
    for (PlacedGlyph& g : l.glyphs)
        g.origin += d;
    for (LayoutLine& ln : l.lines) {
        ln.baseline += d.y;
        ln.x0 += d.x;
        for (LayoutRule& r : ln.rules) {
            r.x0 += d.x;
            r.x1 += d.x;
            r.y += d.y;
        }
    }
    if (!l.frame.empty()) {
        l.frame.min += d;
        l.frame.max += d;
    }
}

void finishBox(TextLayout& l) {
    l.box = l.frame;
    for (const PlacedGlyph& g : l.glyphs)
        l.box.merge(glyphBoxLayout(g));
}

// layout -> world for text: scale x by wf, shear by the oblique angle, rotate, translate.
Transform2 makeTransform(Vec2 at, double rotation, double widthFactor, double oblique, double k) {
    const double c = std::cos(rotation), s = std::sin(rotation), t = std::tan(oblique);
    Transform2 m;
    m.m00 = k * c * widthFactor;
    m.m10 = k * s * widthFactor;
    m.m01 = k * (c * t - s);
    m.m11 = k * (s * t + c);
    m.tx = at.x;
    m.ty = at.y;
    return m;
}

}  // namespace

std::array<Vec2, 4> TextLayout::frameCorners() const {
    if (frame.empty())
        return {};
    return {toWorld.apply({frame.min.x, frame.min.y}), toWorld.apply({frame.max.x, frame.min.y}),
            toWorld.apply({frame.max.x, frame.max.y}), toWorld.apply({frame.min.x, frame.max.y})};
}

BBox2 TextLayout::worldBounds() const {
    return box.empty() ? BBox2{} : toWorld.applyToBox(box);
}

double measureWidth(const TextStyle* style, std::string_view utf8, double height, bool bold,
                    bool italic) {
    Attr a;
    a.height = height;
    a.bold = bold;
    a.italic = italic;
    return shapeChunk(style, utf8, a).advance;
}

static TextLayout layoutTextRaw(const Document& doc, const TextData& d) {
    TextLayout out;
    const TextStyle* style = styleFor(doc, d.style);
    const double styleWf = effectiveWidthFactor(style);
    const double styleObl = style ? style->oblique : 0.0;
    double wf = styleWf * (d.widthFactor > 0 ? d.widthFactor : 1.0);
    const double obl = styleObl + d.oblique;
    const double height = d.height > 0 ? d.height : 2.5;

    Attr attr;
    attr.height = height;
    FontInfo fi = fontInfo(style, attr);
    const double descDepth = -fi.descender * (height / fi.capHeight);

    Line items;
    std::string text = stripInvisible(d.utf8);
    // %%u and %%o switch underline and overline on and off, as in AutoCAD TEXT.
    {
        Attr cur = attr;
        std::string run;
        auto flush = [&] {
            if (!run.empty())
                items.push_back({run, cur, false, 0});
            run.clear();
        };
        for (std::size_t i = 0; i < text.size();) {
            if (text[i] == '%' && i + 2 < text.size() && text[i + 1] == '%') {
                const char k = text[i + 2];
                if (k == 'u' || k == 'U' || k == 'o' || k == 'O') {
                    flush();
                    if (k == 'u' || k == 'U')
                        cur.underline = !cur.underline;
                    else
                        cur.overline = !cur.overline;
                    i += 3;
                    continue;
                }
            }
            run.push_back(text[i++]);
        }
        flush();
    }
    LayoutLine ln = emitLine(style, items, 0, out.glyphs, height);
    ln.height = height;
    out.lines.push_back(ln);
    const double W = ln.width;

    double dx = 0, dy = 0, k = 1, rot = d.rotation;
    Vec2 anchor = d.position;
    const bool spans = d.h == HAlign::Aligned || d.h == HAlign::Fit;
    if (spans) {
        Vec2 v = d.alignPoint - d.position;
        double D = length(v);
        if (D > tol::kLinear && W > tol::kLinear) {
            rot = std::atan2(v.y, v.x);
            if (d.h == HAlign::Aligned)
                k = D / (W * wf);
            else
                wf = D / W;
        }
    } else {
        if (!(d.h == HAlign::Left && d.v == VAlign::Baseline))
            anchor = d.alignPoint;
        switch (d.h) {
            case HAlign::Center:
            case HAlign::Middle:
                dx = -W / 2;
                break;
            case HAlign::Right:
                dx = -W;
                break;
            default:
                break;
        }
        if (d.h == HAlign::Middle) {
            dy = -height / 2;
        } else {
            switch (d.v) {
                case VAlign::Bottom:
                    dy = descDepth;
                    break;
                case VAlign::Middle:
                    dy = -height / 2;
                    break;
                case VAlign::Top:
                    dy = -height;
                    break;
                default:
                    break;
            }
        }
    }

    out.frame.min = {0, -descDepth};
    out.frame.max = {W, height};
    shiftLayout(out, {dx, dy});
    finishBox(out);
    out.toWorld = makeTransform(anchor, rot, wf, obl, k);
    // Backward / upside-down generation: mirror the layout about the text's own axes.
    const bool back = d.backward != (style && style->backward);
    const bool down = d.upsideDown != (style && style->upsideDown);
    if (back || down) {
        Transform2 flip;
        flip.m00 = back ? -1 : 1;
        flip.m11 = down ? -1 : 1;
        out.toWorld = out.toWorld * flip;
    }
    return out;
}

static TextLayout layoutMTextRaw(const Document& doc, const MTextData& d) {
    TextLayout out;
    const TextStyle* style = styleFor(doc, d.style);
    const double wf = effectiveWidthFactor(style);
    const double obl = style ? style->oblique : 0.0;
    const double height = d.height > 0 ? d.height : 2.5;

    std::vector<Piece> pieces = parseMarkup(d.utf8, height);
    // The wrap width is in layout space, i.e. before the style's width factor.
    LineBuilder builder(style, d.width > 0 ? d.width / (wf > 0 ? wf : 1.0) : 0.0);
    std::vector<Piece> para;
    Attr lastAttr;
    lastAttr.height = height;
    auto endParagraph = [&](const Attr& at) {
        bool hasText = false;
        for (const Piece& p : para)
            hasText = hasText || !p.text.empty();
        if (hasText)
            builder.addParagraph(para);
        else
            builder.addEmptyParagraph(at);
        para.clear();
    };
    bool any = false;
    for (const Piece& p : pieces) {
        if (p.newline) {
            endParagraph(p.attr);
            any = true;
        } else {
            para.push_back(p);
            lastAttr = p.attr;
            any = true;
        }
    }
    if (!para.empty() || !any)
        endParagraph(lastAttr);

    // Stack the lines downward from y = 0 (the top of the frame).
    double y = 0, maxW = 0, lastH = height;
    bool first = true;
    for (const Line& line : builder.lines()) {
        double h = 0;
        for (const Item& it : line)
            h = std::max(h, it.attr.height);
        if (h <= 0)
            h = height;
        y -= first ? h : kLineSpacing * h;
        first = false;
        LayoutLine ll = emitLine(style, line, y, out.glyphs, height);
        ll.height = h;
        out.lines.push_back(ll);
        maxW = std::max(maxW, ll.width);
        lastH = h;
    }
    FontInfo fi = fontInfo(style, [&] {
        Attr a;
        a.height = lastH;
        return a;
    }());
    const double descDepth = -fi.descender * (lastH / fi.capHeight);
    const double colW = d.width > 0 ? d.width / (wf > 0 ? wf : 1.0) : maxW;
    double frameW = colW;
    double bottom = y - descDepth;

    // Centre and right attachments also set each line centred or right-aligned in the frame.
    const bool centred = d.attach == Attachment::TopCenter ||
                         d.attach == Attachment::MiddleCenter ||
                         d.attach == Attachment::BottomCenter;
    const bool righted = d.attach == Attachment::TopRight || d.attach == Attachment::MiddleRight ||
                         d.attach == Attachment::BottomRight;
    if (centred || righted)
        for (LayoutLine& ln : out.lines) {
            const double slack = std::max(0.0, colW - ln.width);
            const double move = centred ? slack / 2 : slack;
            for (std::size_t g = ln.firstGlyph; g < ln.endGlyph; ++g)
                out.glyphs[g].origin.x += move;
            ln.x0 += move;
            for (LayoutRule& r : ln.rules) {
                r.x0 += move;
                r.x1 += move;
            }
        }

    // Columns: split the lines into balanced runs and set the runs side by side.
    if (d.columns > 1 && d.width > 0 && out.lines.size() > 1) {
        const std::size_t cols = std::min<std::size_t>(d.columns, out.lines.size());
        const double gap = (d.columnGap > 0 ? d.columnGap : 2 * height) / (wf > 0 ? wf : 1.0);
        std::vector<double> reach;  // distance of each line's bottom below the first baseline
        const double top = out.lines.front().baseline;
        for (const LayoutLine& ln : out.lines)
            reach.push_back(top - ln.baseline);
        const double total = reach.back() + out.lines.back().height;
        const double target = total / static_cast<double>(cols);
        std::size_t col = 0, firstOfCol = 0;
        double shiftY = 0;
        double lowest = top;
        for (std::size_t i = 0; i < out.lines.size(); ++i) {
            const double mid = reach[i] + out.lines[i].height / 2;
            const std::size_t want = std::min(cols - 1, static_cast<std::size_t>(mid / target));
            if (want > col) {
                col = want;
                firstOfCol = i;
                shiftY = reach[firstOfCol];  // lift this column to the top of the frame
            }
            LayoutLine& ln = out.lines[i];
            const double mx = static_cast<double>(col) * (colW + gap), my = shiftY;
            for (std::size_t g = ln.firstGlyph; g < ln.endGlyph; ++g) {
                out.glyphs[g].origin.x += mx;
                out.glyphs[g].origin.y += my;
            }
            ln.baseline += my;
            ln.x0 += mx;
            for (LayoutRule& r : ln.rules) {
                r.x0 += mx;
                r.x1 += mx;
                r.y += my;
            }
            lowest = std::min(lowest, ln.baseline);
        }
        frameW = static_cast<double>(cols) * colW + static_cast<double>(cols - 1) * gap;
        bottom = lowest - descDepth;
    }
    out.frame.min = {0, bottom};
    out.frame.max = {frameW, 0};

    double dx = 0, dy = 0;
    switch (d.attach) {
        case Attachment::TopCenter:
        case Attachment::MiddleCenter:
        case Attachment::BottomCenter:
            dx = -frameW / 2;
            break;
        case Attachment::TopRight:
        case Attachment::MiddleRight:
        case Attachment::BottomRight:
            dx = -frameW;
            break;
        default:
            break;
    }
    switch (d.attach) {
        case Attachment::MiddleLeft:
        case Attachment::MiddleCenter:
        case Attachment::MiddleRight:
            dy = -bottom / 2;
            break;
        case Attachment::BottomLeft:
        case Attachment::BottomCenter:
        case Attachment::BottomRight:
            dy = -bottom;
            break;
        default:
            break;
    }
    shiftLayout(out, {dx, dy});
    finishBox(out);
    out.toWorld = makeTransform(d.position, d.rotation, wf, obl, 1.0);
    return out;
}

TextLayout layoutText(const Document& doc, const TextData& d) {
    if (!hasFields(d.utf8))
        return layoutTextRaw(doc, d);
    TextData c = d;
    c.utf8 = expandFields(doc, d.utf8);
    return layoutTextRaw(doc, c);
}

TextLayout layoutMText(const Document& doc, const MTextData& d) {
    if (!hasFields(d.utf8))
        return layoutMTextRaw(doc, d);
    MTextData c = d;
    c.utf8 = expandFields(doc, d.utf8);
    return layoutMTextRaw(doc, c);
}

TextLayout layoutEntity(const Document& doc, const Entity& e, double annoScale) {
    const double f = annotationFactor(doc.header(), e.common, annoScale);
    if (const auto* t = entityAs<TextData>(e)) {
        if (f == 1.0)
            return layoutText(doc, *t);
        TextData c = *t;
        c.height *= f;
        return layoutText(doc, c);
    }
    if (const auto* m = entityAs<MTextData>(e)) {
        if (f == 1.0)
            return layoutMText(doc, *m);
        MTextData c = *m;
        c.height *= f;
        c.width *= f;
        return layoutMText(doc, c);
    }
    return {};
}

}  // namespace tbcad::text
