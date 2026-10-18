#include "tbcad/dxf/encoding.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "codepage.h"
#include "tbcad/dxf/lexer.h"

namespace tbcad::dxf {

namespace {

using detail::DoubleByteTable;
using detail::kLeadByte;
using detail::kUndefinedChar;
using detail::SingleByteTable;

constexpr char32_t kReplacement = 0xFFFD;

struct NamedEncoding {
    std::string_view name;
    Encoding encoding;
};

constexpr NamedEncoding kNames[] = {
    {"UTF-8", Encoding::Utf8},       {"ANSI_874", Encoding::Cp874},
    {"ANSI_932", Encoding::Cp932},   {"ANSI_936", Encoding::Cp936},
    {"ANSI_949", Encoding::Cp949},   {"ANSI_950", Encoding::Cp950},
    {"ANSI_1361", Encoding::Cp1361}, {"ANSI_1250", Encoding::Cp1250},
    {"ANSI_1251", Encoding::Cp1251}, {"ANSI_1252", Encoding::Cp1252},
    {"ANSI_1253", Encoding::Cp1253}, {"ANSI_1254", Encoding::Cp1254},
    {"ANSI_1255", Encoding::Cp1255}, {"ANSI_1256", Encoding::Cp1256},
    {"ANSI_1257", Encoding::Cp1257}, {"ANSI_1258", Encoding::Cp1258},
};

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

const SingleByteTable* singleTable(Encoding e) {
    switch (e) {
        case Encoding::Cp874:
            return &detail::kCp874;
        case Encoding::Cp1250:
            return &detail::kCp1250;
        case Encoding::Cp1251:
            return &detail::kCp1251;
        case Encoding::Cp1252:
            return &detail::kCp1252;
        case Encoding::Cp1253:
            return &detail::kCp1253;
        case Encoding::Cp1254:
            return &detail::kCp1254;
        case Encoding::Cp1255:
            return &detail::kCp1255;
        case Encoding::Cp1256:
            return &detail::kCp1256;
        case Encoding::Cp1257:
            return &detail::kCp1257;
        case Encoding::Cp1258:
            return &detail::kCp1258;
        default:
            return nullptr;
    }
}

const DoubleByteTable* doubleTable(Encoding e) {
    switch (e) {
        case Encoding::Cp932:
            return &detail::kCp932;
        case Encoding::Cp936:
            return &detail::kCp936;
        case Encoding::Cp949:
            return &detail::kCp949;
        case Encoding::Cp950:
            return &detail::kCp950;
        case Encoding::Cp1361:
            return &detail::kCp1361;
        default:
            return nullptr;
    }
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = kReplacement;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes one UTF-8 sequence at s[i]. Returns its length, or 0 when invalid.
std::size_t decodeUtf8At(std::string_view s, std::size_t i, char32_t& cp) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    std::size_t len = 0;
    char32_t v = 0;
    char32_t minimum = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2, v = c & 0x1F, minimum = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3, v = c & 0x0F, minimum = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4, v = c & 0x07, minimum = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) {
            return 0;
        }
        v = (v << 6) | (b(i + k) & 0x3F);
    }
    if (v < minimum || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        return 0;
    }
    cp = v;
    return len;
}

struct ByteStats {
    std::size_t high = 0;         // bytes >= 0x80
    std::size_t thaiLetters = 0;  // high bytes that are assigned Thai code points in cp874
    std::size_t runs = 0;         // runs of consecutive high bytes
    bool validUtf8 = true;
    std::size_t multibyte = 0;  // UTF-8 multi-byte sequences (when validUtf8)
};

ByteStats analyse(std::span<const std::byte> bytes) {
    ByteStats st;
    const std::string_view s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    bool inRun = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            inRun = false;
            continue;
        }
        ++st.high;
        if (!inRun) {
            ++st.runs;
            inRun = true;
        }
        if (c >= 0xA1 && c <= 0xFB && !(c >= 0xDB && c <= 0xDE)) {
            ++st.thaiLetters;
        }
    }
    // Validate UTF-8 only when there is something to validate.
    for (std::size_t i = 0; i < s.size() && st.validUtf8;) {
        char32_t cp = 0;
        const std::size_t len = decodeUtf8At(s, i, cp);
        if (len == 0) {
            st.validUtf8 = false;
        } else {
            if (len > 1) {
                ++st.multibyte;
            }
            i += len;
        }
    }
    return st;
}

bool looksThai(const ByteStats& st) {
    return st.high >= 4 && st.thaiLetters * 10 >= st.high * 9 && st.high >= 2 * st.runs;
}

// Numeric part of "AC1021"; 0 when unknown.
int acadVersionNumber(std::string_view v) {
    if (v.size() != 6 || v.substr(0, 2) != "AC") {
        return 0;
    }
    int n = 0;
    auto [p, ec] = std::from_chars(v.data() + 2, v.data() + v.size(), n);
    return ec == std::errc() && p == v.data() + v.size() ? n : 0;
}

bool isBinaryDxf(std::span<const std::byte> bytes) {
    constexpr std::string_view kSig = "AutoCAD Binary DXF";
    return bytes.size() >= kSig.size() &&
           std::string_view(reinterpret_cast<const char*>(bytes.data()), kSig.size()) == kSig;
}

}  // namespace

std::string_view encodingName(Encoding e) {
    for (const auto& n : kNames) {
        if (n.encoding == e) {
            return n.name;
        }
    }
    return "ANSI_1252";
}

std::optional<Encoding> encodingFromName(std::string_view dwgCodepage) {
    while (!dwgCodepage.empty() && (dwgCodepage.front() == ' ' || dwgCodepage.front() == '\t')) {
        dwgCodepage.remove_prefix(1);
    }
    while (!dwgCodepage.empty() && (dwgCodepage.back() == ' ' || dwgCodepage.back() == '\t' ||
                                    dwgCodepage.back() == '\r')) {
        dwgCodepage.remove_suffix(1);
    }
    const std::string u = upper(dwgCodepage);
    if (u == "UTF8") {
        return Encoding::Utf8;
    }
    if (u == "ASCII" || u == "8859_1") {
        return Encoding::Cp1252;
    }
    if (u == "8859_2") {
        return Encoding::Cp1250;
    }
    for (const auto& n : kNames) {
        if (u == n.name) {
            return n.encoding;
        }
    }
    return std::nullopt;
}

HeaderHints scanHeaderHints(std::span<const std::byte> bytes) {
    HeaderHints hints;
    Report scratch;
    auto opened = Lexer::open(bytes, scratch);
    if (!opened) {
        return hints;
    }
    Lexer& lx = opened.value();
    bool inHeader = false;
    int variable = 0;  // 1 = reading $ACADVER, 2 = reading $DWGCODEPAGE
    for (int guard = 0; guard < 100000; ++guard) {
        const auto g = lx.next();
        if (!g) {
            break;
        }
        if (g->code == 0) {
            const std::string_view s = g->str();
            if (s == "SECTION") {
                inHeader = false;
            } else if (s == "ENDSEC" && inHeader) {
                break;
            } else if (s == "EOF") {
                break;
            }
            variable = 0;
        } else if (g->code == 2 && !inHeader) {
            if (g->str() == "HEADER") {
                inHeader = true;
            } else {
                break;  // The header is always the first section.
            }
        } else if (inHeader && g->code == 9) {
            const std::string_view name = g->str();
            variable = name == "$ACADVER" ? 1 : name == "$DWGCODEPAGE" ? 2 : 0;
        } else if (inHeader && variable == 1 && g->code == 1) {
            hints.acadVer = std::string(g->str());
            variable = 0;
        } else if (inHeader && variable == 2 && g->code == 3) {
            hints.dwgCodepage = std::string(g->str());
            variable = 0;
        }
        if (!hints.acadVer.empty() && !hints.dwgCodepage.empty()) {
            break;
        }
    }
    return hints;
}

Encoding detectEncoding(std::span<const std::byte> bytes, std::string_view acadVer,
                        std::string_view dwgCodepage, Report& report) {
    // 1. Byte order mark.
    if (bytes.size() >= 3 && bytes[0] == std::byte{0xEF} && bytes[1] == std::byte{0xBB} &&
        bytes[2] == std::byte{0xBF}) {
        return Encoding::Utf8;
    }
    const ByteStats st = analyse(bytes);
    const bool binary = isBinaryDxf(bytes);
    const int version = acadVersionNumber(acadVer);

    // 2. R2007 and later are UTF-8 whatever $DWGCODEPAGE says.
    if (version >= 1021) {
        if (st.validUtf8) {
            return Encoding::Utf8;
        }
        report.warn(code::kEncodingMismatch,
                    "file declares " + std::string(acadVer) +
                        " (UTF-8) but contains invalid UTF-8; falling back to the codepage rules");
    }

    // Nothing to decide for a file of plain ASCII.
    const std::optional<Encoding> declared = encodingFromName(dwgCodepage);
    if (st.high == 0) {
        return declared.value_or(Encoding::Cp1252);
    }
    if (!dwgCodepage.empty() && !declared) {
        report.warn(code::kEncodingUnknown,
                    "unknown $DWGCODEPAGE \"" + std::string(dwgCodepage) + "\", guessing");
    }

    // The binary payload of a binary DXF is not text; do not sniff it.
    const bool unreliableDeclaration = !declared || *declared == Encoding::Cp1252;
    if (declared && !unreliableDeclaration) {
        return *declared;
    }

    // 3/4. Declared ANSI_1252, or nothing usable declared: look at the bytes.
    if (!binary && st.validUtf8 && st.multibyte > 0) {
        report.info(code::kEncodingGuess,
                    std::string("text is valid UTF-8, treating the file as UTF-8 (declared: ") +
                        (declared ? std::string(encodingName(*declared)) : "none") + ")");
        return Encoding::Utf8;
    }
    if (!binary && looksThai(st)) {
        if (declared) {
            report.warn(code::kEncodingMismatch,
                        "$DWGCODEPAGE is ANSI_1252 but the text looks like Thai; using ANSI_874");
        } else {
            report.info(code::kEncodingGuess,
                        "no usable $DWGCODEPAGE and the text looks like Thai; using ANSI_874");
        }
        return Encoding::Cp874;
    }
    if (!declared) {
        report.info(code::kEncodingGuess, "no usable $DWGCODEPAGE; using ANSI_1252");
    }
    return Encoding::Cp1252;
}

std::string decodeText(std::string_view raw, Encoding from) {
    std::string out;
    out.reserve(raw.size() + raw.size() / 2);
    const SingleByteTable* sb = singleTable(from);
    const DoubleByteTable* db = doubleTable(from);
    const std::size_t n = raw.size();

    auto hex = [&](std::size_t at, std::size_t count, std::uint32_t& value) {
        if (at + count > n) {
            return false;
        }
        const auto [p, ec] = std::from_chars(raw.data() + at, raw.data() + at + count, value, 16);
        return ec == std::errc() && p == raw.data() + at + count;
    };
    auto isUnicodeEscape = [&](std::size_t at, std::uint32_t& value) {
        return at + 2 < n && raw[at] == '\\' && (raw[at + 1] == 'U' || raw[at + 1] == 'u') &&
               raw[at + 2] == '+' && hex(at + 3, 4, value);
    };

    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\\') {
            std::uint32_t v = 0;
            if (isUnicodeEscape(i, v)) {
                i += 7;
                if (v >= 0xD800 && v <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (isUnicodeEscape(i, low) && low >= 0xDC00 && low <= 0xDFFF) {
                        i += 7;
                        appendUtf8(out, 0x10000 + ((v - 0xD800) << 10) + (low - 0xDC00));
                        continue;
                    }
                }
                appendUtf8(out, v);
                continue;
            }
            if (i + 8 <= n && raw[i + 1] == 'M' && raw[i + 2] == '+' && raw[i + 3] >= '1' &&
                raw[i + 3] <= '5') {
                std::uint32_t pair = 0;
                if (hex(i + 4, 4, pair)) {
                    const Encoding enc = raw[i + 3] == '1'   ? Encoding::Cp932
                                         : raw[i + 3] == '2' ? Encoding::Cp950
                                         : raw[i + 3] == '3' ? Encoding::Cp949
                                         : raw[i + 3] == '4' ? Encoding::Cp1361
                                                             : Encoding::Cp936;
                    const DoubleByteTable* t = doubleTable(enc);
                    const unsigned lead = pair >> 8;
                    const unsigned trail = pair & 0xFF;
                    char16_t u = 0;
                    if (lead >= t->leadMin && lead <= t->leadMax && trail >= t->trailMin &&
                        trail <= t->trailMax) {
                        u = t->pairs[(lead - t->leadMin) * (t->trailMax - t->trailMin + 1) +
                                     (trail - t->trailMin)];
                    }
                    appendUtf8(out, u != 0 ? u : kReplacement);
                    i += 8;
                    continue;
                }
            }
            out.push_back('\\');
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if (from == Encoding::Utf8) {
            char32_t cp = 0;
            const std::size_t len = decodeUtf8At(raw, i, cp);
            if (len == 0) {
                appendUtf8(out, kReplacement);
                ++i;
            } else {
                out.append(raw.substr(i, len));
                i += len;
            }
            continue;
        }
        if (sb != nullptr) {
            appendUtf8(out, sb->high[c - 0x80]);
            ++i;
            continue;
        }
        const char16_t entry = db->high[c - 0x80];
        if (entry != kLeadByte) {
            appendUtf8(out, entry);
            ++i;
            continue;
        }
        char16_t u = 0;
        bool trailInRange = false;
        if (i + 1 < n) {
            const unsigned trail = static_cast<unsigned char>(raw[i + 1]);
            if (c >= db->leadMin && c <= db->leadMax && trail >= db->trailMin &&
                trail <= db->trailMax) {
                trailInRange = true;
                u = db->pairs[(c - db->leadMin) * (db->trailMax - db->trailMin + 1) +
                              (trail - db->trailMin)];
            }
        }
        if (u != 0) {
            appendUtf8(out, u);
            i += 2;
        } else {
            appendUtf8(out, kReplacement);
            i += trailInRange ? 2 : 1;
        }
    }
    return out;
}

std::string replacePercentCodes(std::string_view s, bool keepRules) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '%' || i + 2 >= s.size() || s[i + 1] != '%') {
            out.push_back(s[i++]);
            continue;
        }
        const char k = s[i + 2];
        switch (k) {
            case 'c':
            case 'C':
                appendUtf8(out, 0x2300);
                i += 3;
                continue;
            case 'd':
            case 'D':
                appendUtf8(out, 0x00B0);
                i += 3;
                continue;
            case 'p':
            case 'P':
                appendUtf8(out, 0x00B1);
                i += 3;
                continue;
            case '%':
                out.push_back('%');
                i += 3;
                continue;
            case 'u':
            case 'U':
            case 'o':
            case 'O':
                if (keepRules) {  // TEXT underlines and overlines itself
                    out.append(s.substr(i, 3));
                    i += 3;
                    continue;
                }
                i += 3;
                continue;
            case 'k':
            case 'K':
                i += 3;
                continue;
            default:
                break;
        }
        if (i + 5 <= s.size()) {
            unsigned v = 0;
            const auto [p, ec] = std::from_chars(s.data() + i + 2, s.data() + i + 5, v);
            if (ec == std::errc() && p == s.data() + i + 5 && v > 0 && v < 256) {
                appendUtf8(out, v);
                i += 5;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string stripMText(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        const char c = s[i];
        if (c == '{' || c == '}') {
            ++i;
            continue;
        }
        if (c != '\\') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + 1 >= n) {
            break;  // lone trailing backslash
        }
        const char d = s[i + 1];
        switch (d) {
            case 'P':
                out.push_back('\n');
                i += 2;
                break;
            case '\\':
            case '{':
            case '}':
                out.push_back(d);
                i += 2;
                break;
            case '~':
                appendUtf8(out, 0x00A0);
                i += 2;
                break;
            case 'L':
            case 'l':
            case 'O':
            case 'o':
            case 'K':
            case 'k':
                i += 2;
                break;
            case 'f':
            case 'F':
            case 'H':
            case 'h':
            case 'C':
            case 'c':
            case 'T':
            case 't':
            case 'Q':
            case 'q':
            case 'W':
            case 'w':
            case 'A':
            case 'a':
            case 'p': {
                const std::size_t semi = s.find(';', i + 2);
                i = semi == std::string_view::npos ? i + 2 : semi + 1;
                break;
            }
            case 'S': {
                const std::size_t semi = s.find(';', i + 2);
                if (semi == std::string_view::npos) {
                    i += 2;
                    break;
                }
                char separator = 0;
                for (std::size_t k = i + 2; k < semi; ++k) {
                    if (s[k] == '\\' && k + 1 < semi) {
                        out.push_back(s[++k]);
                    } else if (separator == 0 && (s[k] == '/' || s[k] == '#' || s[k] == '^')) {
                        separator = s[k];
                        out.push_back(separator == '^' ? ' ' : '/');
                    } else {
                        out.push_back(s[k]);
                    }
                }
                i = semi + 1;
                break;
            }
            default:
                out.push_back(d);
                i += 2;
                break;
        }
    }
    return replacePercentCodes(out);
}

}  // namespace tbcad::dxf
