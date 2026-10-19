#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "pdf_parse.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::underlay {

namespace {

using pdf::File;
using pdf::Lexer;
using pdf::Obj;

constexpr double kPtToMm = 25.4 / 72.0;
constexpr int kMaxFormDepth = 8;
constexpr std::size_t kMaxOps = 30'000'000;

struct Mat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec2 apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
};
// Applies `m` first, then `n`.
Mat mul(const Mat& m, const Mat& n) {
    return {m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d,       m.c * n.a + m.d * n.c,
            m.c * n.b + m.d * n.d, m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

struct Rgb {
    double r = 0, g = 0, b = 0;
};

struct State {
    Mat ctm{};
    Rgb stroke{}, fill{};
};

struct OutPoly {
    std::vector<Vec2> pts{};
    bool closed = false;
    Rgb color{};
    int layer = -1;  // index into Interpreter::layers; -1 = outside any optional content
};

// Name of an optional-content group: PDF text strings are PDFDocEncoding or UTF-16BE.
std::string pdfText(const std::string& s) {
    std::string out;
    if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xFE &&
        static_cast<unsigned char>(s[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < s.size(); i += 2) {
            const unsigned cp =
                (static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]);
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }
    return s;
}

struct Sub {
    std::vector<Vec2> pts{};
    bool closed = false;
};

class Interpreter {
public:
    Interpreter(const File& file, std::size_t maxVertices)
        : file_(file), maxVertices_(maxVertices) {}

    void run(std::string_view content, const Obj& resources, State st, int depth);

    std::vector<OutPoly> out;
    std::size_t vertices = 0, textRuns = 0, images = 0, skippedFills = 0;
    bool truncated = false;
    BBox2 page{};
    // Optional-content groups met so far: name and whether the file switches them off.
    struct Group {
        std::string name{};
        bool off = false;
    };
    std::vector<Group> groups{};
    std::set<int> offRefs{};  // object numbers of the groups the default configuration hides

private:
    const File& file_;
    std::size_t maxVertices_;
    std::size_t ops_ = 0;

    int curLayer_ = -1;
    std::map<int, int> groupOfRef_{};
    std::map<std::string, int> groupOfName_{};
    int groupFor(const Obj& ref, int depth = 0);
    void emit(const Sub& s, bool stroke, bool fill, const State& st);
    void flatten(Sub& s, Vec2 p1, Vec2 p2, Vec2 p3, const State& st);
};

Rgb fromCmyk(double c, double m, double y, double k) {
    return {(1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k)};
}

double num(const std::vector<Obj>& v, std::size_t i) {
    return i < v.size() && v[i].isNum() ? v[i].num : 0;
}

void Interpreter::flatten(Sub& s, Vec2 p1, Vec2 p2, Vec2 p3, const State&) {
    Vec2 p0 = s.pts.empty() ? p1 : s.pts.back();
    double len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) +
                 std::hypot(p3.x - p2.x, p3.y - p2.y);
    int n = std::clamp(static_cast<int>(std::ceil(len / 1.5)), 4, 40);
    for (int i = 1; i <= n; ++i) {
        double t = static_cast<double>(i) / n, u = 1 - t;
        double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        s.pts.push_back({w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                         w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y});
    }
}

void Interpreter::emit(const Sub& s, bool stroke, bool fill, const State& st) {
    if (s.pts.size() < 2 || (!stroke && !fill))
        return;
    if (vertices + s.pts.size() > maxVertices_) {
        truncated = true;
        return;
    }
    Rgb color = stroke ? st.stroke : st.fill;
    if (!stroke && fill) {
        // A white fill is paper, and a fill that covers the whole page is its background.
        if (color.r > 0.97 && color.g > 0.97 && color.b > 0.97) {
            ++skippedFills;
            return;
        }
        BBox2 b;
        for (Vec2 p : s.pts)
            b.expand(p);
        if (!page.empty() && b.width() >= 0.9 * page.width() && b.height() >= 0.9 * page.height()) {
            ++skippedFills;
            return;
        }
    }
    OutPoly o;
    o.pts = s.pts;
    o.closed = s.closed || fill;
    o.color = color;
    o.layer = curLayer_;
    vertices += o.pts.size();
    out.push_back(std::move(o));
}

// The layer of an optional-content group or membership dictionary (-1 when it names none).
int Interpreter::groupFor(const Obj& ref, int depth) {
    if (depth > 4)
        return -1;
    if (ref.type == Obj::Type::Ref) {
        auto it = groupOfRef_.find(ref.refNum);
        if (it != groupOfRef_.end())
            return it->second;
    }
    const Obj& d = file_.resolve(ref);
    if (!d.isDict())
        return -1;
    int idx = -1;
    if (const Obj* groupsEntry = d.get("OCGs")) {  // membership dictionary: its first group
        const Obj& g = file_.resolve(*groupsEntry);
        if (g.isArr())
            idx = g.arr.empty() ? -1 : groupFor(g.arr[0], depth + 1);
        else
            idx = groupFor(*groupsEntry, depth + 1);
    } else {
        const Obj* nm = file_.find(d, "Name");
        std::string name = nm ? pdfText(nm->str) : std::string();
        if (name.empty())
            name = "Layer " + std::to_string(groups.size() + 1);
        auto byName = groupOfName_.find(name);
        if (byName != groupOfName_.end()) {
            idx = byName->second;
        } else {
            idx = static_cast<int>(groups.size());
            groups.push_back({name, ref.type == Obj::Type::Ref && offRefs.count(ref.refNum) > 0});
            groupOfName_[name] = idx;
        }
    }
    if (ref.type == Obj::Type::Ref)
        groupOfRef_[ref.refNum] = idx;
    return idx;
}

void Interpreter::run(std::string_view content, const Obj& resources, State st, int depth) {
    std::vector<State> stack;
    std::vector<int> marked;  // layer to return to at each EMC
    const int entryLayer = curLayer_;
    std::vector<Obj> args;
    std::vector<Sub> path;
    Vec2 cur{};
    Vec2 subStart{};
    bool pendingClip = false;
    Lexer lx(content);

    auto paint = [&](bool stroke, bool fill, bool close) {
        if (close && !path.empty() && !path.back().pts.empty())
            path.back().closed = true;
        for (const Sub& s : path)
            emit(s, stroke, fill, st);
        path.clear();
        pendingClip = false;
    };
    auto ensureSub = [&]() -> Sub& {
        if (path.empty())
            path.emplace_back();
        return path.back();
    };
    auto colorOps = [&](Rgb& target) {
        if (args.size() == 1 && args[0].isNum())
            target = {args[0].num, args[0].num, args[0].num};
        else if (args.size() == 3)
            target = {num(args, 0), num(args, 1), num(args, 2)};
        else if (args.size() == 4)
            target = fromCmyk(num(args, 0), num(args, 1), num(args, 2), num(args, 3));
    };

    for (;;) {
        Obj o;
        bool kw = false;
        if (!lx.next(o, kw, false))
            break;
        if (++ops_ > kMaxOps || truncated)
            break;
        if (!kw || o.type != Obj::Type::Name) {
            if (args.size() < 64)
                args.push_back(std::move(o));
            continue;
        }
        const std::string& op = o.str;
        auto A = [&](std::size_t i) { return num(args, i); };
        if (op == "q") {
            if (stack.size() < 64)
                stack.push_back(st);
        } else if (op == "Q") {
            if (!stack.empty()) {
                st = stack.back();
                stack.pop_back();
            }
        } else if (op == "cm") {
            if (args.size() >= 6)
                st.ctm = mul(Mat{A(0), A(1), A(2), A(3), A(4), A(5)}, st.ctm);
        } else if (op == "m") {
            if (args.size() >= 2) {
                path.emplace_back();
                cur = st.ctm.apply(A(0), A(1));
                subStart = cur;
                path.back().pts.push_back(cur);
            }
        } else if (op == "l") {
            if (args.size() >= 2) {
                cur = st.ctm.apply(A(0), A(1));
                ensureSub().pts.push_back(cur);
            }
        } else if (op == "c" || op == "v" || op == "y") {
            Vec2 p1, p2, p3;
            bool ok = true;
            if (op == "c" && args.size() >= 6) {
                p1 = st.ctm.apply(A(0), A(1));
                p2 = st.ctm.apply(A(2), A(3));
                p3 = st.ctm.apply(A(4), A(5));
            } else if (op == "v" && args.size() >= 4) {
                p1 = cur;
                p2 = st.ctm.apply(A(0), A(1));
                p3 = st.ctm.apply(A(2), A(3));
            } else if (op == "y" && args.size() >= 4) {
                p1 = st.ctm.apply(A(0), A(1));
                p3 = st.ctm.apply(A(2), A(3));
                p2 = p3;
            } else {
                ok = false;
            }
            if (ok) {
                Sub& s = ensureSub();
                if (s.pts.empty())
                    s.pts.push_back(cur);
                flatten(s, p1, p2, p3, st);
                cur = p3;
            }
        } else if (op == "h") {
            if (!path.empty() && !path.back().pts.empty()) {
                path.back().closed = true;
                cur = subStart;
            }
        } else if (op == "re") {
            if (args.size() >= 4) {
                double x = A(0), y = A(1), w = A(2), h = A(3);
                Sub s;
                s.pts = {st.ctm.apply(x, y), st.ctm.apply(x + w, y), st.ctm.apply(x + w, y + h),
                         st.ctm.apply(x, y + h)};
                s.closed = true;
                cur = subStart = s.pts[0];
                path.push_back(std::move(s));
            }
        } else if (op == "S") {
            paint(true, false, false);
        } else if (op == "s") {
            paint(true, false, true);
        } else if (op == "f" || op == "F" || op == "f*") {
            if (pendingClip)
                paint(false, false, false);
            else
                paint(false, true, false);
        } else if (op == "B" || op == "B*") {
            paint(true, false, false);
        } else if (op == "b" || op == "b*") {
            paint(true, false, true);
        } else if (op == "n") {
            path.clear();
            pendingClip = false;
        } else if (op == "W" || op == "W*") {
            pendingClip = true;
        } else if (op == "g" || op == "rg" || op == "k") {
            colorOps(st.fill);
        } else if (op == "G" || op == "RG" || op == "K") {
            colorOps(st.stroke);
        } else if (op == "sc" || op == "scn") {
            if (!args.empty() && args.back().isNum())
                colorOps(st.fill);
        } else if (op == "SC" || op == "SCN") {
            if (!args.empty() && args.back().isNum())
                colorOps(st.stroke);
        } else if (op == "BDC" || op == "BMC") {
            marked.push_back(curLayer_);
            if (op == "BDC" && args.size() >= 2 && args[0].isName("OC")) {
                int g = -1;
                if (args[1].isName()) {
                    const Obj* props = file_.find(resources, "Properties");
                    const Obj* ref = props && props->isDict() ? props->get(args[1].str) : nullptr;
                    if (ref)
                        g = groupFor(*ref);
                } else if (args[1].isDict()) {
                    g = groupFor(args[1]);
                }
                if (g >= 0)
                    curLayer_ = g;
            }
        } else if (op == "EMC") {
            if (!marked.empty()) {
                curLayer_ = marked.back();
                marked.pop_back();
            }
        } else if (op == "BT") {
            ++textRuns;
        } else if (op == "BI") {
            // Inline image: skip to "EI".
            ++images;
            std::size_t p = lx.pos();
            for (;;) {
                std::size_t e = content.find("EI", p);
                if (e == std::string_view::npos) {
                    lx.seek(content.size());
                    break;
                }
                bool before = e == 0 || content[e - 1] == '\n' || content[e - 1] == '\r' ||
                              content[e - 1] == ' ';
                bool after = e + 2 >= content.size() || content[e + 2] == '\n' ||
                             content[e + 2] == '\r' || content[e + 2] == ' ';
                if (before && after) {
                    lx.seek(e + 2);
                    break;
                }
                p = e + 2;
            }
        } else if (op == "Do") {
            if (!args.empty() && args[0].isName() && depth < kMaxFormDepth) {
                const Obj* xo = file_.find(resources, "XObject");
                const Obj* xe = xo ? file_.find(*xo, args[0].str) : nullptr;
                if (xe && xe->type == Obj::Type::Stream) {
                    const Obj* sub = file_.find(*xe, "Subtype");
                    if (sub && sub->isName("Form")) {
                        State fs = st;
                        if (const Obj* m = file_.find(*xe, "Matrix");
                            m && m->isArr() && m->arr.size() >= 6) {
                            auto g = [&](int i) {
                                const Obj& r = file_.resolve(m->arr[i]);
                                return r.isNum() ? r.num : 0.0;
                            };
                            fs.ctm = mul(Mat{g(0), g(1), g(2), g(3), g(4), g(5)}, fs.ctm);
                        }
                        std::vector<std::uint8_t> bytes = file_.decode(*xe);
                        const Obj* fr = file_.find(*xe, "Resources");
                        const int before = curLayer_;
                        if (const Obj* ocRef = xe->get("OC")) {
                            const int g = groupFor(*ocRef);
                            if (g >= 0)
                                curLayer_ = g;
                        }
                        run(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                             bytes.size()),
                            fr ? *fr : resources, fs, depth + 1);
                        curLayer_ = before;
                    } else if (sub && sub->isName("Image")) {
                        ++images;
                    }
                }
            }
        }
        args.clear();
    }
    curLayer_ = entryLayer;
}

std::string lowerStr(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

double boxNum(const File& f, const Obj& arr, std::size_t i) {
    if (i >= arr.arr.size())
        return 0;
    const Obj& r = f.resolve(arr.arr[i]);
    return r.isNum() ? r.num : 0;
}

}  // namespace

Result<std::unique_ptr<Document>> importPdf(std::span<const std::byte> bytes,
                                            const PdfOptions& options, PdfReport* report) {
    PdfReport local;
    PdfReport& rep = report ? *report : local;
    rep = PdfReport{};
    std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    File file;
    std::string error;
    if (!file.open(sv, error))
        return makeError(ErrorCode::Parse, error);
    rep.pageCount = static_cast<int>(file.pages().size());
    if (options.page < 1 || options.page > rep.pageCount)
        return makeError(ErrorCode::InvalidArgument,
                         "The PDF has " + std::to_string(rep.pageCount) + " page(s); page " +
                             std::to_string(options.page) + " does not exist");
    const Obj& page = file.pages()[static_cast<std::size_t>(options.page - 1)].dict;

    // Page box, rotation, unit.
    double x0 = 0, y0 = 0, x1 = 612, y1 = 792;
    for (const char* key : {"CropBox", "MediaBox"}) {
        const Obj* b = file.find(page, key);
        if (b && b->isArr() && b->arr.size() >= 4) {
            x0 = std::min(boxNum(file, *b, 0), boxNum(file, *b, 2));
            x1 = std::max(boxNum(file, *b, 0), boxNum(file, *b, 2));
            y0 = std::min(boxNum(file, *b, 1), boxNum(file, *b, 3));
            y1 = std::max(boxNum(file, *b, 1), boxNum(file, *b, 3));
            break;
        }
    }
    double unit = kPtToMm;
    if (const Obj* u = file.find(page, "UserUnit"); u && u->isNum() && u->num > 0)
        unit *= u->num;
    int rot = 0;
    if (const Obj* r = file.find(page, "Rotate"); r && r->isNum())
        rot = ((static_cast<int>(std::lround(r->num)) % 360) + 360) % 360;
    const double w = x1 - x0, h = y1 - y0;
    Mat base;  // PDF user space -> millimetres, lower-left of the (rotated) page at the origin
    switch (rot) {
        case 90:
            base = {0, -1, 1, 0, -y0, x1};
            break;  // (x,y) -> (y - y0, x1 - x)
        case 180:
            base = {-1, 0, 0, -1, x1, y1};
            break;
        case 270:
            base = {0, 1, -1, 0, y1, -x0};
            break;  // (x,y) -> (y1 - y, x - x0)
        default:
            base = {1, 0, 0, 1, -x0, -y0};
            break;
    }
    base = mul(base, Mat{unit, 0, 0, unit, 0, 0});
    const bool swap = rot == 90 || rot == 270;
    rep.widthMm = (swap ? h : w) * unit;
    rep.heightMm = (swap ? w : h) * unit;

    Interpreter in(file, options.maxVertices);
    in.page.expand({0, 0});
    in.page.expand({rep.widthMm, rep.heightMm});

    // Content streams.
    std::string content;
    if (const Obj* c = file.find(page, "Contents")) {
        auto append = [&](const Obj& s) {
            std::vector<std::uint8_t> b = file.decode(s);
            content.append(reinterpret_cast<const char*>(b.data()), b.size());
            content.push_back('\n');
        };
        if (c->type == Obj::Type::Stream) {
            append(*c);
        } else if (c->isArr()) {
            for (const Obj& e : c->arr)
                append(file.resolve(e));
        }
    }
    const Obj* resources = file.find(page, "Resources");
    // Groups the default configuration hides: all of them with BaseState OFF (except those in
    // ON), else those in OFF.
    if (const Obj* ocp = file.find(file.root(), "OCProperties")) {
        auto refsOf = [&](const Obj* arr, auto&& fn) {
            if (!arr)
                return;
            const Obj& a = file.resolve(*arr);
            if (a.isArr())
                for (const Obj& e : a.arr)
                    if (e.type == Obj::Type::Ref)
                        fn(e.refNum);
        };
        if (const Obj* cfg = file.find(*ocp, "D")) {
            const Obj* base = file.find(*cfg, "BaseState");
            if (base && base->isName("OFF")) {
                refsOf(ocp->get("OCGs"), [&](int n) { in.offRefs.insert(n); });
                refsOf(cfg->get("ON"), [&](int n) { in.offRefs.erase(n); });
            }
            refsOf(cfg->get("OFF"), [&](int n) { in.offRefs.insert(n); });
        }
    }
    State st;
    st.ctm = base;
    in.run(content, resources ? *resources : Obj{}, st, 0);
    for (const Interpreter::Group& g : in.groups)
        rep.layers.push_back(g.name);

    rep.paths = in.out.size();
    rep.vertices = in.vertices;
    rep.textRuns = in.textRuns;
    rep.images = in.images;
    rep.skippedFills = in.skippedFills;
    rep.truncated = in.truncated;
    if (in.textRuns)
        rep.notes.push_back(std::to_string(in.textRuns) + " text object(s) are not drawn");
    if (in.images)
        rep.notes.push_back(std::to_string(in.images) + " raster image(s) are not drawn");
    if (in.truncated)
        rep.notes.push_back("The page has more geometry than the limit; the rest is left out");

    // Document: one layer, one polyline per path, colours faded toward a neutral grey.
    auto doc = std::make_unique<Document>();
    {
        auto tx = doc->begin("PDF page");
        Layer layer;
        layer.name = kPdfLayerName;
        auto lid = tx.addLayer(layer);
        if (!lid)
            return lid.error();
        // One layer per optional-content group; the ones the file hides start switched off.
        std::vector<LayerId> groupLayers;
        for (const Interpreter::Group& g : in.groups) {
            std::string name = g.name;
            for (char& c : name)
                if (std::strchr("<>/\\\":;?*|=`", c) || static_cast<unsigned char>(c) < 32)
                    c = '_';
            while (!name.empty() && name.back() == ' ')
                name.pop_back();
            if (name.empty())
                name = "Layer";
            LayerId found{};
            for (std::size_t i = 0; i < groupLayers.size(); ++i)
                if (const Layer* l = doc->layer(groupLayers[i]); l && l->name == name)
                    found = groupLayers[i];
            if (!found) {
                Layer l;
                l.name = name;
                l.on = !g.off;
                auto r = tx.addLayer(l);
                if (!r)
                    return r.error();
                found = r.value();
            }
            groupLayers.push_back(found);
        }
        const double fade = std::clamp(options.fadePercent, 0.0, 95.0) / 100.0;
        constexpr double kGrey = 160;
        auto chan = [&](double v) {
            double c = std::clamp(v, 0.0, 1.0) * 255.0;
            return static_cast<std::uint8_t>(std::lround(c + (kGrey - c) * fade));
        };
        for (const OutPoly& o : in.out) {
            PolylineData pl;
            pl.closed = o.closed;
            pl.v.reserve(o.pts.size());
            for (Vec2 p : o.pts)
                pl.v.push_back({p, 0, 0, 0});
            Entity e;
            e.common.layer =
                o.layer >= 0 ? groupLayers[static_cast<std::size_t>(o.layer)] : lid.value();
            e.common.color = Color::rgb(chan(o.color.r), chan(o.color.g), chan(o.color.b));
            e.data = std::move(pl);
            auto added = tx.add(std::move(e));
            if (!added)
                return added.error();
        }
        tx.commit();
    }
    return doc;
}

Result<std::unique_ptr<Document>> importPdfFile(const std::filesystem::path& file,
                                                const PdfOptions& options, PdfReport* report) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::NotFound, "Cannot open the file " + file.string());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return importPdf(std::as_bytes(std::span<const char>(raw)), options, report);
}

Result<int> pdfPageCount(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::NotFound, "Cannot open the file " + file.string());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    File f;
    std::string error;
    if (!f.open(std::string_view(raw.data(), raw.size()), error))
        return makeError(ErrorCode::Parse, error);
    return static_cast<int>(f.pages().size());
}

// ---- stored path spec ---------------------------------------------------------------------

Spec parseSpec(std::string_view stored) {
    Spec s;
    s.file = std::string(stored);
    std::size_t hash = stored.rfind('#');
    if (hash == std::string_view::npos || hash == 0)
        return s;
    std::string_view tail = stored.substr(hash + 1);
    // Only "key=number" pairs separated by commas count as a spec.
    PdfOptions opt;
    std::size_t i = 0;
    bool any = false;
    while (i < tail.size()) {
        std::size_t comma = tail.find(',', i);
        std::string_view part =
            tail.substr(i, comma == std::string_view::npos ? std::string_view::npos : comma - i);
        std::size_t eq = part.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 >= part.size())
            return s;
        std::string key = lowerStr(part.substr(0, eq));
        std::string val(part.substr(eq + 1));
        char* end = nullptr;
        double v = std::strtod(val.c_str(), &end);
        if (end == val.c_str() || *end != 0)
            return s;
        if (key == "page")
            opt.page = std::max(1, static_cast<int>(v));
        else if (key == "fade")
            opt.fadePercent = std::clamp(v, 0.0, 95.0);
        any = true;
        if (comma == std::string_view::npos)
            break;
        i = comma + 1;
    }
    if (!any)
        return s;
    s.file = std::string(stored.substr(0, hash));
    s.pdf = opt;
    s.hasSpec = true;
    return s;
}

std::string makeSpec(std::string_view file, const PdfOptions& options) {
    PdfOptions def;
    std::string out(file);
    std::string spec;
    auto add = [&](const std::string& kv) {
        spec += spec.empty() ? "" : ",";
        spec += kv;
    };
    auto fmt = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return std::string(buf);
    };
    if (options.page != def.page)
        add("page=" + std::to_string(options.page));
    if (options.fadePercent != def.fadePercent)
        add("fade=" + fmt(options.fadePercent));
    if (!spec.empty())
        out += "#" + spec;
    return out;
}

bool isPdfPath(std::string_view stored) {
    std::string f = lowerStr(parseSpec(stored).file);
    return f.size() >= 4 && f.compare(f.size() - 4, 4, ".pdf") == 0;
}

}  // namespace tbcad::underlay
