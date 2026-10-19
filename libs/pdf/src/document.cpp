#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <ostream>
#include <sstream>
#include <thread>
#include <vector>

#include "miniz.h"
#include "tbcad/pdf/pdf.h"
#include "truetype.h"

namespace tbcad::pdf {
namespace {

constexpr double kPtPerMm = 72.0 / 25.4;

// PDF text string: PDFDocEncoding for pure ASCII, else UTF-16BE with BOM as a hex string.
std::string textString(std::string_view utf8) {
    bool ascii = true;
    for (unsigned char c : utf8)
        ascii = ascii && c < 0x80;
    std::string out;
    if (ascii) {
        out += '(';
        for (char c : utf8) {
            if (c == '(' || c == ')' || c == '\\')
                out += '\\';
            if (c == '\r' || c == '\n') {
                out += (c == '\r') ? "\\r" : "\\n";
                continue;
            }
            out += c;
        }
        out += ')';
        return out;
    }
    auto hex4 = [&](unsigned v) {
        static const char* d = "0123456789ABCDEF";
        for (int sh = 12; sh >= 0; sh -= 4)
            out += d[(v >> sh) & 0xF];
    };
    out += "<FEFF";
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0xFFFD;
        int len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c >> 4) == 0xE) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c >> 3) == 0x1E) {
            len = 4;
            cp = c & 0x07;
        }
        if (len > 1) {
            if (i + len > utf8.size()) {
                cp = 0xFFFD;
                len = static_cast<int>(utf8.size() - i);
            } else {
                for (int k = 1; k < len; ++k) {
                    unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
                    if ((cc >> 6) != 0x2) {
                        cp = 0xFFFD;
                        break;
                    }
                    cp = (cp << 6) | (cc & 0x3F);
                }
            }
        }
        i += len;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            hex4(0xD800 + (cp >> 10));
            hex4(0xDC00 + (cp & 0x3FF));
        } else {
            hex4(cp > 0xFFFF ? 0xFFFD : cp);
        }
    }
    out += '>';
    return out;
}

std::string xrefEntry(size_t offset) {
    char buf[32];
    // Exactly 20 bytes: 10-digit offset, space, 5-digit generation, space, 'n', CR LF.
    std::snprintf(buf, sizeof buf, "%010zu 00000 n\r\n", offset);
    return buf;
}

// zlib stream of `raw`; empty on failure.
// zlib stream of `raw` made by deflating pieces on several threads. Each piece ends on a byte
// boundary (full flush), so the pieces simply follow one another inside one ordinary zlib stream
// that any reader inflates; the cost is a slightly larger result than one pass would give.
std::string deflateParallel(const std::string& raw) {
    constexpr std::size_t kPiece = std::size_t{1} << 20;
    const std::size_t pieces = (raw.size() + kPiece - 1) / kPiece;
    const int flags = static_cast<int>(
        tdefl_create_comp_flags_from_zip_params(6, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY));
    auto one = [&](std::size_t i) {
        struct Sink {
            std::string out;
        } sink;
        tdefl_compressor* d = new tdefl_compressor;
        tdefl_init(
            d,
            [](const void* b, int n, void* user) {
                static_cast<Sink*>(user)->out.append(static_cast<const char*>(b),
                                                     static_cast<std::size_t>(n));
                return 1;
            },
            &sink, static_cast<mz_uint>(flags));
        const std::size_t at = i * kPiece;
        tdefl_compress_buffer(d, raw.data() + at, std::min(kPiece, raw.size() - at),
                              TDEFL_FULL_FLUSH);
        delete d;
        return std::move(sink.out);
    };
    std::vector<std::future<std::string>> jobs;
    jobs.reserve(pieces);
    const std::size_t lanes = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    std::string out = "\x78\x9c";  // zlib header: deflate, 32K window, default level
    for (std::size_t i = 0; i < pieces; ++i) {
        jobs.push_back(std::async(std::launch::async, one, i));
        // At most `lanes` pieces are in flight; the oldest is collected first.
        if (jobs.size() >= lanes) {
            out += jobs.front().get();
            jobs.erase(jobs.begin());
        }
    }
    for (auto& j : jobs)
        out += j.get();
    out += std::string("\x03\x00", 2);  // final empty block
    const mz_ulong ad =
        mz_adler32(MZ_ADLER32_INIT, reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    for (int sh = 24; sh >= 0; sh -= 8)
        out += static_cast<char>((ad >> sh) & 0xFF);
    return out;
}

std::string deflate(const std::string& raw) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string z(bound, '\0');
    mz_ulong zlen = bound;
    if (mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &zlen,
                     reinterpret_cast<const unsigned char*>(raw.data()),
                     static_cast<mz_ulong>(raw.size()), 6) != MZ_OK)
        return {};
    z.resize(zlen);
    return z;
}

// UTF-16BE hex digits of a string of code points.
std::string utf16Hex(std::u32string_view text) {
    std::string out;
    auto hex4 = [&](unsigned v) {
        static const char* d = "0123456789ABCDEF";
        for (int sh = 12; sh >= 0; sh -= 4)
            out += d[(v >> sh) & 0xF];
    };
    for (char32_t cp : text) {
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            hex4(0xD800 + (cp >> 10));
            hex4(0xDC00 + (cp & 0x3FF));
        } else {
            hex4(cp > 0xFFFF ? 0xFFFD : static_cast<unsigned>(cp));
        }
    }
    return out;
}

std::string hex4(unsigned v) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (int sh = 12; sh >= 0; sh -= 4)
        out += d[(v >> sh) & 0xF];
    return out;
}

// PDF name characters: letters, digits and a few safe marks; anything else becomes '-'.
std::string pdfName(std::string_view s) {
    std::string out;
    for (char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
                   ? c
                   : '-';
    return out.empty() ? std::string("Font") : out;
}

}  // namespace

bool canEmbedTrueType(const std::vector<unsigned char>& file, int faceIndex) {
    return detail::readTrueType(file, faceIndex).has_value();
}

int PdfDocument::addFont(EmbeddedFont font) {
    fonts_.push_back(std::move(font));
    return static_cast<int>(fonts_.size()) - 1;
}

void PdfDocument::setInfo(std::string_view title, std::string_view creator) {
    title_ = title;
    creator_ = creator;
}

int PdfDocument::addLayer(std::string_view name) {
    layerNames_.emplace_back(name);
    return static_cast<int>(layerNames_.size()) - 1;
}

Page& PdfDocument::addPage(double widthMm, double heightMm) {
    pages_.push_back(Page(widthMm, heightMm));
    return pages_.back();
}

Result<void> PdfDocument::write(std::ostream& out) const {
    // Object numbering: 1 catalog, 2 pages, 3 info, then (page, content) pairs.
    const size_t pageObj0 = 4;
    std::string buf;
    std::vector<size_t> offsets;  // offsets[i] = byte offset of object i+1

    // Optional content (layers) needs PDF 1.5.
    buf += layerNames_.empty() ? "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n" : "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";
    auto begin = [&](size_t num) {
        offsets.push_back(buf.size());
        buf += std::to_string(num) + " 0 obj\n";
    };
    auto end = [&] { buf += "endobj\n"; };
    // Pictures come after the pages: each distinct pixel buffer is an image object, followed by a
    // soft mask object when it has transparent pixels.
    struct PicObj {
        size_t image = 0, mask = 0;
    };
    std::map<const ImageData*, PicObj> picObjs;
    size_t nextObj = pageObj0 + 2 * pages_.size();
    for (const Page& p : pages_)
        for (const Page::Pic& pic : p.pics_) {
            if (picObjs.count(pic.data.get()))
                continue;
            PicObj o;
            o.image = nextObj++;
            const auto& px = pic.data->rgba;
            bool alpha = false;
            for (size_t k = 3; k < px.size() && !alpha; k += 4)
                alpha = px[k] != 255;
            if (alpha)
                o.mask = nextObj++;
            picObjs[pic.data.get()] = o;
        }

    const size_t ocgObj0 = nextObj;
    nextObj += layerNames_.size();

    // Fonts the pages use: Type0 font, CIDFontType2, descriptor, font file and ToUnicode each.
    std::map<int, std::map<std::uint16_t, std::u32string>> glyphs;
    for (const Page& p : pages_)
        for (const auto& [f, used] : p.glyphsUsed_)
            for (const auto& [g, text] : used) {
                auto& slot = glyphs[f][g];
                if (slot.empty())
                    slot = text;
            }
    std::map<int, size_t> fontObj;
    for (const auto& [f, used] : glyphs) {
        if (f < 0 || static_cast<size_t>(f) >= fonts_.size() || !fonts_[static_cast<size_t>(f)].file)
            return makeError(ErrorCode::InvalidArgument, "page uses an unknown font");
        fontObj[f] = nextObj;
        nextObj += 5;
    }

    auto resources = [&](const Page& p) {
        std::string props;
        if (!p.layersUsed_.empty()) {
            props = "/Properties << ";
            for (int k : p.layersUsed_)
                props += "/L" + std::to_string(k) + " " + std::to_string(ocgObj0 + k) + " 0 R ";
            props += ">> ";
        }
        if (!p.fontsUsed_.empty()) {
            props += "/Font << ";
            for (int f : p.fontsUsed_)
                props += "/F" + std::to_string(f) + " " + std::to_string(fontObj.at(f)) + " 0 R ";
            props += ">> ";
        }
        if (p.pics_.empty())
            return props.empty() ? std::string("<< >>") : "<< " + props + ">>";
        std::string xo, gs;
        for (size_t k = 0; k < p.pics_.size(); ++k) {
            xo += "/Im" + std::to_string(k) + " " +
                  std::to_string(picObjs.at(p.pics_[k].data.get()).image) + " 0 R ";
            gs += "/Gs" + std::to_string(k) + " << /ca " + formatNumber(p.pics_[k].opacity) +
                  " /CA " + formatNumber(p.pics_[k].opacity) + " >> ";
        }
        return "<< " + props + "/XObject << " + xo + ">> /ExtGState << " + gs + ">> >>";
    };

    begin(1);
    buf += "<< /Type /Catalog /Pages 2 0 R";
    if (!layerNames_.empty()) {
        std::string refs;
        for (size_t k = 0; k < layerNames_.size(); ++k)
            refs += std::to_string(ocgObj0 + k) + " 0 R ";
        buf += " /OCProperties << /OCGs [" + refs + "] /D << /Order [" + refs + "] >> >>";
    }
    buf += " >>\n";
    end();

    begin(2);
    buf += "<< /Type /Pages /Count " + std::to_string(pages_.size()) + " /Kids [";
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (i)
            buf += ' ';
        buf += std::to_string(pageObj0 + 2 * i) + " 0 R";
    }
    buf += "] >>\n";
    end();

    begin(3);
    buf += "<< /Producer (TBCAD) /Creator " + textString(creator_);
    if (!title_.empty())
        buf += " /Title " + textString(title_);
    if (!creationDate_.empty())
        buf += " /CreationDate " + textString(creationDate_);
    buf += " >>\n";
    end();

    for (size_t i = 0; i < pages_.size(); ++i) {
        const Page& p = pages_[i];
        const size_t pageNum = pageObj0 + 2 * i;
        begin(pageNum);
        // Whole points, as AutoCAD writes them (A1 is 2384 x 1684 pt, 841.0 x 594.1 mm).
        buf += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
               formatNumber(std::max(1.0, std::round(p.widthMm() * kPtPerMm))) + " " +
               formatNumber(std::max(1.0, std::round(p.heightMm() * kPtPerMm))) +
               "] /Resources " + resources(p) + " /Contents " + std::to_string(pageNum + 1) +
               " 0 R >>\n";
        end();

        // Content: one page-level cm so all drawing is in mm.
        std::string raw = "q\n" + formatNumber(kPtPerMm, 8) + " 0 0 " + formatNumber(kPtPerMm, 8) +
                          " 0 0 cm\n" + p.content() + "Q\n";
        std::string payload = raw;
        bool deflated = false;
        if (compress_ && raw.size() > (std::size_t{2} << 20)) {
            payload = deflateParallel(raw);
            deflated = true;
        } else if (compress_) {
            mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
            std::string z(bound, '\0');
            mz_ulong zlen = bound;
            int rc = mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &zlen,
                                  reinterpret_cast<const unsigned char*>(raw.data()),
                                  static_cast<mz_ulong>(raw.size()), 6);
            if (rc != MZ_OK)
                return makeError(ErrorCode::Io, "deflate failed");
            z.resize(zlen);
            payload = std::move(z);
            deflated = true;
        }
        begin(pageNum + 1);
        buf += "<< /Length " + std::to_string(payload.size()) +
               (deflated ? " /Filter /FlateDecode" : "") + " >>\nstream\n";
        buf += payload;
        buf += "\nendstream\n";
        end();
    }

    // Object numbers are the order of the offsets table, so write them by number.
    std::vector<std::pair<const ImageData*, PicObj>> ordered(picObjs.begin(), picObjs.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& l, const auto& r) { return l.second.image < r.second.image; });
    for (const auto& [data, o] : ordered) {
        const auto& px = data->rgba;
        const size_t n = static_cast<size_t>(data->width) * static_cast<size_t>(data->height);
        std::string rgb(n * 3, '\0'), a(n, '\0');
        for (size_t k = 0; k < n; ++k) {
            rgb[3 * k] = static_cast<char>(px[4 * k]);
            rgb[3 * k + 1] = static_cast<char>(px[4 * k + 1]);
            rgb[3 * k + 2] = static_cast<char>(px[4 * k + 2]);
            a[k] = static_cast<char>(px[4 * k + 3]);
        }
        const std::string z = deflate(rgb);
        if (z.empty())
            return makeError(ErrorCode::Io, "deflate failed");
        const std::string dims =
            "/Width " + std::to_string(data->width) + " /Height " + std::to_string(data->height);
        begin(o.image);
        buf += "<< /Type /XObject /Subtype /Image " + dims +
               " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Interpolate true" +
               (o.mask ? " /SMask " + std::to_string(o.mask) + " 0 R" : std::string()) +
               " /Filter /FlateDecode /Length " + std::to_string(z.size()) + " >>\nstream\n";
        buf += z;
        buf += "\nendstream\n";
        end();
        if (o.mask) {
            const std::string za = deflate(a);
            if (za.empty())
                return makeError(ErrorCode::Io, "deflate failed");
            begin(o.mask);
            buf += "<< /Type /XObject /Subtype /Image " + dims +
                   " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length " +
                   std::to_string(za.size()) + " >>\nstream\n";
            buf += za;
            buf += "\nendstream\n";
            end();
        }
    }

    for (size_t k = 0; k < layerNames_.size(); ++k) {
        begin(ocgObj0 + k);
        buf += "<< /Type /OCG /Name " + textString(layerNames_[k]) + " >>\n";
        end();
    }

    for (const auto& [f, used] : glyphs) {
        const EmbeddedFont& ef = fonts_[static_cast<size_t>(f)];
        auto info = detail::readTrueType(*ef.file, ef.faceIndex);
        if (!info)
            return makeError(ErrorCode::InvalidArgument,
                             "font " + ef.postscriptName + " cannot be embedded");
        std::set<std::uint16_t> keep;
        for (const auto& [g, text] : used)
            keep.insert(g);
        const std::string subset = detail::subsetTrueType(*ef.file, ef.faceIndex, keep);
        if (subset.empty())
            return makeError(ErrorCode::InvalidArgument,
                             "font " + ef.postscriptName + " cannot be subset");
        // Subset tag: six capitals from the font number and glyph set, as PDF asks of subsets.
        std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(f);
        for (std::uint16_t g : keep)
            h = (h ^ g) * 16777619u;
        std::string tag;
        for (int k = 0; k < 6; ++k) {
            tag += static_cast<char>('A' + h % 26);
            h /= 26;
        }
        std::string ps = ef.postscriptName.empty() ? info->postscriptName : ef.postscriptName;
        if (ps.empty())
            ps = "Font" + std::to_string(f);
        const std::string name = tag + "+" + pdfName(ps);
        const double k1000 = 1000.0 / info->unitsPerEm;
        auto u = [&](double v) { return formatNumber(std::round(v * k1000)); };
        const size_t o = fontObj.at(f);

        begin(o);
        buf += "<< /Type /Font /Subtype /Type0 /BaseFont /" + name +
               " /Encoding /Identity-H /DescendantFonts [" + std::to_string(o + 1) +
               " 0 R] /ToUnicode " + std::to_string(o + 4) + " 0 R >>\n";
        end();

        std::string widths;
        for (const auto& [g, text] : used) {
            const double adv = g < info->advances.size() ? info->advances[g] : 0;
            widths += std::to_string(g) + " [" + u(adv) + "] ";
        }
        begin(o + 1);
        buf += "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + name +
               " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
               " /FontDescriptor " + std::to_string(o + 2) +
               " 0 R /CIDToGIDMap /Identity /DW 1000 /W [" + widths + "] >>\n";
        end();

        begin(o + 2);
        const int flags = (info->fixedPitch ? 1 : 0) | 4 | (info->italicAngle != 0 ? 64 : 0);
        buf += "<< /Type /FontDescriptor /FontName /" + name + " /Flags " +
               std::to_string(flags) + " /FontBBox [" + u(info->xMin) + " " + u(info->yMin) +
               " " + u(info->xMax) + " " + u(info->yMax) + "] /ItalicAngle " +
               formatNumber(info->italicAngle) + " /Ascent " + u(info->ascent) + " /Descent " +
               u(info->descent) + " /CapHeight " + u(info->capHeight) +
               " /StemV 80 /FontFile2 " + std::to_string(o + 3) + " 0 R >>\n";
        end();

        const std::string zf = deflate(subset);
        if (zf.empty())
            return makeError(ErrorCode::Io, "deflate failed");
        begin(o + 3);
        buf += "<< /Length " + std::to_string(zf.size()) + " /Length1 " +
               std::to_string(subset.size()) + " /Filter /FlateDecode >>\nstream\n";
        buf += zf;
        buf += "\nendstream\n";
        end();

        std::string cmap =
            "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::pair<std::uint16_t, std::u32string>> mapped;
        for (const auto& [g, text] : used)
            if (!text.empty())
                mapped.emplace_back(g, text);
        for (size_t i = 0; i < mapped.size(); i += 100) {
            const size_t n = std::min<size_t>(100, mapped.size() - i);
            cmap += std::to_string(n) + " beginbfchar\n";
            for (size_t k = i; k < i + n; ++k)
                cmap += "<" + hex4(mapped[k].first) + "> <" + utf16Hex(mapped[k].second) + ">\n";
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        begin(o + 4);
        buf += "<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap +
               "\nendstream\n";
        end();
    }

    const size_t xrefPos = buf.size();
    buf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n";
    buf += "0000000000 65535 f\r\n";
    for (size_t off : offsets)
        buf += xrefEntry(off);
    buf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) +
           " /Root 1 0 R /Info 3 0 R >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!out)
        return makeError(ErrorCode::Io, "write to stream failed");
    return {};
}

Result<void> PdfDocument::writeFile(const std::string& path) const {
    const std::u8string u8(path.begin(), path.end());
    std::ofstream f(std::filesystem::path(u8), std::ios::binary | std::ios::trunc);
    if (!f)
        return makeError(ErrorCode::Io, "cannot open for writing: " + path);
    Result<void> r = write(f);
    if (!r)
        return r;
    f.close();
    if (!f)
        return makeError(ErrorCode::Io, "write failed: " + path);
    return {};
}

}  // namespace tbcad::pdf
