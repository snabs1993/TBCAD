#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

#include "miniz.h"
#include "tbcad/pdf/pdf.h"

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

}  // namespace

void PdfDocument::setInfo(std::string_view title, std::string_view creator) {
    title_ = title;
    creator_ = creator;
}

Page& PdfDocument::addPage(double widthMm, double heightMm) {
    pages_.push_back(Page(widthMm, heightMm));
    return pages_.back();
}

Result PdfDocument::write(std::ostream& out) const {
    // Object numbering: 1 catalog, 2 pages, 3 info, then (page, content) pairs.
    const size_t pageObj0 = 4;
    std::string buf;
    std::vector<size_t> offsets;  // offsets[i] = byte offset of object i+1

    buf += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    auto begin = [&](size_t num) {
        offsets.push_back(buf.size());
        buf += std::to_string(num) + " 0 obj\n";
    };
    auto end = [&] { buf += "endobj\n"; };

    begin(1);
    buf += "<< /Type /Catalog /Pages 2 0 R >>\n";
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
        buf += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
               formatNumber(p.widthMm() * kPtPerMm) + " " + formatNumber(p.heightMm() * kPtPerMm) +
               "] /Resources << >> /Contents " + std::to_string(pageNum + 1) + " 0 R >>\n";
        end();

        // Content: one page-level cm so all drawing is in mm.
        std::string raw = "q\n" + formatNumber(kPtPerMm, 8) + " 0 0 " + formatNumber(kPtPerMm, 8) +
                          " 0 0 cm\n" + p.content() + "Q\n";
        std::string payload = raw;
        bool deflated = false;
        if (compress_) {
            mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
            std::string z(bound, '\0');
            mz_ulong zlen = bound;
            int rc = mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &zlen,
                                  reinterpret_cast<const unsigned char*>(raw.data()),
                                  static_cast<mz_ulong>(raw.size()), 6);
            if (rc != MZ_OK)
                return Result::failure("deflate failed");
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

    const size_t xrefPos = buf.size();
    buf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n";
    buf += "0000000000 65535 f\r\n";
    for (size_t off : offsets)
        buf += xrefEntry(off);
    buf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) +
           " /Root 1 0 R /Info 3 0 R >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!out)
        return Result::failure("write to stream failed");
    return Result::success();
}

Result PdfDocument::writeFile(const std::string& path) const {
    const std::u8string u8(path.begin(), path.end());
    std::ofstream f(std::filesystem::path(u8), std::ios::binary | std::ios::trunc);
    if (!f)
        return Result::failure("cannot open for writing: " + path);
    Result r = write(f);
    if (!r)
        return r;
    f.close();
    if (!f)
        return Result::failure("write failed: " + path);
    return Result::success();
}

}  // namespace tbcad::pdf
