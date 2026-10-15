#include "container/sections.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bits/bit_reader.h"

namespace tbcad::dwg::detail {

namespace {

std::uint32_t le32(Bytes b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

constexpr std::uint8_t kClassStart[16] = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
                                          0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A};
constexpr std::uint8_t kHeaderStart[16] = {0xCF, 0x7B, 0x1F, 0x23, 0xFD, 0xDE, 0x38, 0xA9,
                                           0x5F, 0x7C, 0x68, 0xB8, 0x4E, 0x6D, 0x33, 0x5F};

// Sentinel, size, and the optional extra size word; returns the data bytes or empty.
Bytes sectionData(Bytes section, const std::uint8_t (&sentinel)[16], std::size_t extraBytes,
                  std::vector<std::string>& notes, const char* what) {
    if (section.size() < 20) {
        notes.push_back(std::string("the ") + what + " section is too short");
        return {};
    }
    if (std::memcmp(section.data(), sentinel, 16) != 0)
        notes.push_back(std::string("the ") + what + " section does not start with its marker");
    std::size_t pos = 16;
    const std::uint32_t size = le32(section, pos);
    pos += 4;
    if (extraBytes) {
        if (section.size() < pos + extraBytes) {
            notes.push_back(std::string("the ") + what + " section is too short");
            return {};
        }
        pos += extraBytes;
    }
    if (size > section.size() - pos) {
        notes.push_back(std::string("the ") + what + " section is cut off");
        return {};
    }
    return section.subspan(pos, size);
}

}  // namespace

namespace {

std::vector<ClassInfo> parseClassesWith(Bytes section, const Format& fmt, std::size_t extra,
                                        std::vector<std::string>& notes) {
    std::vector<ClassInfo> out;
    const Bytes data = sectionData(section, kClassStart, extra, notes, "class");
    if (data.empty())
        return out;
    // R2007 has a long for the largest class number and one flag; from R2010 on a short and
    // two bytes before the flag.
    auto classHeader = [&](BitReader& h) {
        if (fmt.r2010) {
            const std::uint32_t m = h.bs();
            h.rc();
            h.rc();
            h.b();
            return m;
        }
        const std::uint32_t m = h.bl();
        h.b();
        return m;
    };
    BitReader r(data);
    const std::uint32_t maxClass = classHeader(r);  // maximum class number
    const std::size_t count = maxClass >= 500 && maxClass < 100000 ? maxClass - 499 : 0;
    if (!fmt.r2007) {
        // Older releases: names inline with the fields.
        while (r.bitsLeft() >= 16 && !r.failed() && out.size() < count) {
            ClassInfo c;
            c.number = r.bs();
            r.bs();  // proxy flags
            c.app = r.textBytes();
            c.cppName = r.textBytes();
            c.dxfName = r.textBytes();
            r.b();  // was a zombie
            c.entity = r.bs() == 0x1F2;
            r.bl();  // objects of this class
            r.bs();  // version
            r.bs();  // maintenance release
            r.bl();
            r.bl();
            if (r.failed() || c.number < 500)
                break;
            out.push_back(std::move(c));
        }
        return out;
    }
    // R2007 and later: all the fields of every class come first, then one run of UTF-16
    // strings (application, C++ name and DXF name per class) that starts right after them
    // (checked against a real R2018 file; the end-of-data flag of object records is not there).
    // The spec gives the version and maintenance fields as BS in one place and BL in another;
    // the reading whose class numbers run 500, 501, ... and whose strings all fit wins.
    std::vector<ClassInfo> best;
    std::vector<std::string> bestNotes;
    int bestScore = -1;
    for (int variant = 0; variant < 2; ++variant) {
        BitReader fr(data, 0);
        classHeader(fr);
        std::vector<ClassInfo> got;
        bool sequential = true;
        for (std::size_t i = 0; i < count && !fr.failed(); ++i) {
            ClassInfo c;
            c.number = fr.bs();
            fr.bs();  // proxy flags
            fr.b();   // was a zombie
            c.entity = fr.bs() == 0x1F2;
            fr.bl();  // objects of this class
            if (variant == 0) {
                fr.bs();  // version
                fr.bs();  // maintenance release
            } else {
                fr.bl();
                fr.bl();
            }
            fr.bl();
            fr.bl();
            if (fr.failed() || c.number < 500)
                break;
            sequential = sequential && c.number == 500 + static_cast<int>(i);
            got.push_back(std::move(c));
        }
        std::vector<std::string> n;
        BitReader strings(data, fr.bitPos());
        bool namesFit = true;
        for (ClassInfo& c : got) {
            c.app = strings.textUnicode();
            c.cppName = strings.textUnicode();
            c.dxfName = strings.textUnicode();
            if (strings.failed()) {
                n.push_back("the class names are cut off");
                namesFit = false;
                break;
            }
        }
        if (got.size() != count)
            n.push_back("the class section lists " + std::to_string(got.size()) + " of " +
                        std::to_string(count) + " classes (field layout " +
                        std::to_string(variant) + ")");
        const int score =
            static_cast<int>(got.size()) * 4 + (namesFit ? 2 : 0) + (sequential ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            best = std::move(got);
            bestNotes = std::move(n);
        }
    }
    if (best.size() != count) {
        std::string hex;
        char b[4];
        for (std::size_t i = 0; i < data.size() && i < 160; ++i) {
            std::snprintf(b, sizeof b, "%02X", data[i]);
            hex += b;
        }
        bestNotes.push_back("class data starts " + hex);
    }
    out = std::move(best);
    notes.insert(notes.end(), bestNotes.begin(), bestNotes.end());
    return out;
}

}  // namespace

std::vector<ClassInfo> parseClasses(Bytes section, const Format& fmt, int maintenance,
                                    std::vector<std::string>& notes) {
    // The size of the words between the size and the data depends on the release (a real R2018
    // file has two); the first reading that yields classes wins.
    const std::size_t known =
        fmt.r2018 ? 8 : (((fmt.r2010 && maintenance > 3) || (fmt.r2007 && !fmt.r2010)) ? 4 : 0);
    std::vector<std::size_t> tries{known};
    if (fmt.r2007)
        for (std::size_t x : {8u, 4u, 0u})
            if (x != known)
                tries.push_back(x);
    std::vector<std::string> first;
    for (std::size_t i = 0; i < tries.size(); ++i) {
        std::vector<std::string> n;
        auto a = parseClassesWith(section, fmt, tries[i], n);
        if (!a.empty()) {
            if (i == 0)
                notes.insert(notes.end(), n.begin(), n.end());
            return a;
        }
        if (i == 0)
            first = n;
    }
    notes.insert(notes.end(), first.begin(), first.end());
    return {};
}

namespace {
HeaderVars parseHeaderWith(Bytes section, const Format& fmt, std::size_t extraBytes,
                           std::vector<std::string>& notes);
}

HeaderVars parseHeader(Bytes section, const Format& fmt, int maintenance,
                       std::vector<std::string>& notes) {
    const bool extra = fmt.r2018 || (fmt.r2010 && maintenance > 3);
    if (fmt.r2007 && !fmt.r2010) {
        // R2007: whether a second size word follows the first is not documented.
        std::vector<std::string> first;
        HeaderVars v = parseHeaderWith(section, fmt, 4, first);
        if (v.valid) {
            notes.insert(notes.end(), first.begin(), first.end());
            return v;
        }
        return parseHeaderWith(section, fmt, 0, notes);
    }
    return parseHeaderWith(section, fmt, extra ? 4 : 0, notes);
}

namespace {
HeaderVars parseHeaderWith(Bytes section, const Format& fmt, std::size_t extraBytes,
                           std::vector<std::string>& notes) {
    HeaderVars v;
    const Bytes data = sectionData(section, kHeaderStart, extraBytes, notes, "header");
    if (data.empty())
        return v;
    std::vector<std::string> tried;
    if (!fmt.r2007) {
        BitReader r(data);
        parseHeaderVariables(r, fmt, v, notes);
        return v;
    }
    // R2007 and later: strings are in a stream at the end of the data. The way that stream is
    // found is the one used for objects; if the result is not plausible the strings are tried
    // inline. Neither has been confirmed against a real file.
    {
        BitReader r(data);
        BitReader strings;
        if (locateStringStream(data, data.size() * 8, strings)) {
            if (strings.bitSize() != 0)
                r.setStringStream(&strings);
            HeaderVars a;
            parseHeaderVariables(r, fmt, a, tried);
            if (a.valid)
                return a;
        }
    }
    {
        BitReader r(data);
        r.setStringStream(&r);  // UTF-16 strings in the main stream
        HeaderVars b;
        parseHeaderVariables(r, fmt, b, tried);
        if (b.valid)
            return b;
    }
    notes.push_back("the drawing units could not be read from the header; unitless assumed");
    for (const std::string& t : tried)
        if (t.rfind("header ", 0) == 0)
            notes.push_back(t);
    return v;
}

}  // namespace

}  // namespace tbcad::dwg::detail
