#include <miniz.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/export/plot.h"

namespace tbcad::exportlib {

namespace {

std::string num(double v) {
    if (!std::isfinite(v))
        return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

std::string hex(Rgba c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

class Svg {
public:
    explicit Svg(const PlotSheet& sheet) : sheet_(sheet), h_(sheet.layout.pageHeightMm) {}

    std::string run() {
        const PlotLayout& lay = sheet_.layout;
        out_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out_ << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(lay.pageWidthMm)
             << "mm\" height=\"" << num(lay.pageHeightMm) << "mm\" viewBox=\"0 0 "
             << num(lay.pageWidthMm) << ' ' << num(lay.pageHeightMm) << "\">\n";
        out_ << "<defs><clipPath id=\"printable\"><rect x=\"" << num(lay.printable.min.x)
             << "\" y=\"" << num(h_ - lay.printable.max.y) << "\" width=\""
             << num(lay.printable.width()) << "\" height=\"" << num(lay.printable.height())
             << "\"/></clipPath></defs>\n";
        out_ << "<rect width=\"" << num(lay.pageWidthMm) << "\" height=\"" << num(lay.pageHeightMm)
             << "\" fill=\"#ffffff\"/>\n";
        out_ << "<g clip-path=\"url(#printable)\" fill=\"none\" stroke-linejoin=\"round\">\n";
        for (const PlotPrimitive& prim : sheet_.prims) {
            if (const auto* p = std::get_if<PlotPath>(&prim))
                path(*p);
            else if (const auto* a = std::get_if<PlotArc>(&prim))
                arc(*a);
            else if (const auto* f = std::get_if<PlotFill>(&prim))
                fill(*f);
            else if (const auto* d = std::get_if<PlotDots>(&prim))
                dots(*d);
            else if (const auto* im = std::get_if<PlotImage>(&prim))
                image(*im);
            else if (const auto* c = std::get_if<PlotClip>(&prim))
                clip(*c);
        }
        out_ << "</g>\n</svg>\n";
        return out_.str();
    }

private:
    std::string xy(Vec2 p) const { return num(p.x) + ' ' + num(h_ - p.y); }

    std::string strokeAttrs(const PlotStyle& st, bool round) const {
        std::string s =
            " stroke=\"" + hex(st.color) + "\" stroke-width=\"" + num(st.widthMm) + "\"";
        if (!st.dashMm.empty()) {
            s += " stroke-dasharray=\"";
            for (std::size_t i = 0; i < st.dashMm.size(); ++i)
                s += (i ? " " : "") + num(st.dashMm[i]);
            s += "\"";
        } else if (round) {
            s += " stroke-linecap=\"round\"";
        }
        return s;
    }

    // The picture as an embedded PNG, turned and scaled to its place by one matrix.
    void image(const PlotImage& im) {
        const ImageData& d = *im.data;
        if (d.width <= 0 || d.height <= 0)
            return;
        std::size_t size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(d.rgba.data(), d.width, d.height, 4,
                                                               &size, 6, MZ_FALSE);
        if (!png)
            return;
        static const char* kTable =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto* b = static_cast<const unsigned char*>(png);
        std::string enc;
        enc.reserve((size + 2) / 3 * 4);
        for (std::size_t i = 0; i < size; i += 3) {
            const std::uint32_t v = (std::uint32_t{b[i]} << 16) |
                                    (i + 1 < size ? std::uint32_t{b[i + 1]} << 8 : 0) |
                                    (i + 2 < size ? b[i + 2] : 0);
            enc += kTable[(v >> 18) & 63];
            enc += kTable[(v >> 12) & 63];
            enc += i + 1 < size ? kTable[(v >> 6) & 63] : '=';
            enc += i + 2 < size ? kTable[v & 63] : '=';
        }
        mz_free(png);
        const Vec2 tl = im.corners[3], tr = im.corners[2], bl = im.corners[0];
        // Sheet y points up, the SVG y down.
        const Vec2 ex = (tr - tl) / static_cast<double>(d.width);
        const Vec2 ey = (bl - tl) / static_cast<double>(d.height);
        out_ << "<image width=\"" << d.width << "\" height=\"" << d.height << "\" opacity=\""
             << num(im.opacity) << "\" transform=\"matrix(" << num(ex.x) << ' ' << num(-ex.y) << ' '
             << num(ey.x) << ' ' << num(-ey.y) << ' ' << num(tl.x) << ' ' << num(h_ - tl.y)
             << ")\" preserveAspectRatio=\"none\" href=\"data:image/png;base64," << enc << "\"/>\n";
    }

    void clip(const PlotClip& c) {
        if (!c.begin) {
            out_ << "</g>\n";
            return;
        }
        const std::string id = "vp" + std::to_string(++clips_);
        if (c.poly.size() >= 3) {
            out_ << "<clipPath id=\"" << id << "\"><polygon clip-rule=\"evenodd\" points=\"";
            for (std::size_t i = 0; i < c.poly.size(); ++i)
                out_ << (i ? " " : "") << xy(c.poly[i]);
            out_ << "\"/></clipPath>\n<g clip-path=\"url(#" << id << ")\">\n";
            return;
        }
        out_ << "<clipPath id=\"" << id << "\"><rect x=\"" << num(c.rect.min.x) << "\" y=\""
             << num(h_ - c.rect.max.y) << "\" width=\"" << num(c.rect.width()) << "\" height=\""
             << num(c.rect.height()) << "\"/></clipPath>\n<g clip-path=\"url(#" << id << ")\">\n";
    }

    void path(const PlotPath& p) {
        out_ << "<path d=\"M" << xy(p.pts[0]);
        for (std::size_t i = 1; i < p.pts.size(); ++i)
            out_ << " L" << xy(p.pts[i]);
        if (p.closed)
            out_ << " Z";
        out_ << '"' << strokeAttrs(p.style, true) << "/>\n";
    }

    void arc(const PlotArc& a) {
        if (a.full) {
            out_ << "<circle cx=\"" << num(a.center.x) << "\" cy=\"" << num(h_ - a.center.y)
                 << "\" r=\"" << num(a.radius) << '"' << strokeAttrs(a.style, false) << "/>\n";
            return;
        }
        Vec2 s{a.center.x + a.radius * std::cos(a.start),
               a.center.y + a.radius * std::sin(a.start)};
        Vec2 e{a.center.x + a.radius * std::cos(a.end), a.center.y + a.radius * std::sin(a.end)};
        const bool large = (a.end - a.start) > kPi;
        // y is flipped, so counter-clockwise (in the drawing) is sweep-flag 0.
        out_ << "<path d=\"M" << xy(s) << " A" << num(a.radius) << ' ' << num(a.radius) << " 0 "
             << (large ? 1 : 0) << " 0 " << xy(e) << '"' << strokeAttrs(a.style, false) << "/>\n";
    }

    void fill(const PlotFill& f) {
        out_ << "<path fill-rule=\"evenodd\" stroke=\"none\" fill=\"" << hex(f.color) << "\" d=\"";
        for (const auto& ring : f.rings) {
            out_ << 'M' << xy(ring[0]);
            for (std::size_t i = 1; i < ring.size(); ++i)
                out_ << " L" << xy(ring[i]);
            out_ << " Z ";
        }
        out_ << "\"/>\n";
    }

    void dots(const PlotDots& d) {
        for (const Vec2& p : d.pts)
            out_ << "<circle cx=\"" << num(p.x) << "\" cy=\"" << num(h_ - p.y) << "\" r=\""
                 << num(d.style.widthMm / 2) << "\" fill=\"" << hex(d.style.color)
                 << "\" stroke=\"none\"/>\n";
    }

    const PlotSheet& sheet_;
    double h_;
    int clips_ = 0;
    std::ostringstream out_;
};

}  // namespace

std::string svgString(const PlotSheet& sheet) {
    return Svg(sheet).run();
}

Result<void> writeSvg(const PlotSheet& sheet, const std::string& path) {
    const std::u8string u8(path.begin(), path.end());
    std::ofstream f(std::filesystem::path(u8), std::ios::binary);
    if (!f)
        return Error{ErrorCode::Io, "Cannot write " + path};
    f << svgString(sheet);
    if (!f)
        return Error{ErrorCode::Io, "Write failed: " + path};
    return {};
}

}  // namespace tbcad::exportlib
