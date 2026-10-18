#include "tbcad/dxf/lexer.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>

namespace tbcad::dxf {

namespace {

constexpr std::string_view kBinarySentinel{"AutoCAD Binary DXF\r\n\x1a\0", 22};
constexpr int kMinGroupCode = -5;
constexpr int kMaxGroupCode = 1071;

// Words that may follow a "0" group code. Used to resynchronise after a broken line.
constexpr std::string_view kKeywords[] = {
    "3DFACE",     "3DSOLID",     "ACAD_PROXY_ENTITY",
    "APPID",      "ARC",         "ATTDEF",
    "ATTRIB",     "BLOCK",       "BLOCK_RECORD",
    "BODY",       "CIRCLE",      "CLASS",
    "CLASSES",    "DIMENSION",   "DIMSTYLE",
    "ELLIPSE",    "ENDBLK",      "ENDSEC",
    "ENDTAB",     "EOF",         "HATCH",
    "HEADER",     "IMAGE",       "INSERT",
    "LAYER",      "LEADER",      "LINE",
    "LTYPE",      "LWPOLYLINE",  "MLINE",
    "MTEXT",      "OBJECT",      "OBJECTS",
    "POINT",      "POLYLINE",    "RAY",
    "REGION",     "SECTION",     "SEQEND",
    "SHAPE",      "SOLID",       "SPLINE",
    "STYLE",      "TABLE",       "TABLES",
    "TEXT",       "TOLERANCE",   "TRACE",
    "UCS",        "VERTEX",      "VIEW",
    "VIEWPORT",   "VPORT",       "XLINE",
    "ENTITIES",   "BLOCKS",      "THUMBNAILIMAGE",
    "DICTIONARY", "XRECORD",     "LAYOUT",
    "WIPEOUT",    "MULTILEADER", "MLEADER",
};

bool isKeyword(std::string_view s) {
    return std::find(std::begin(kKeywords), std::end(kKeywords), s) != std::end(kKeywords);
}

bool isBlank(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (isBlank(s.front()) || s.front() == '\0')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (isBlank(s.back()) || s.back() == '\0')) {
        s.remove_suffix(1);
    }
    return s;
}

int toLine(std::size_t n) {
    return n > static_cast<std::size_t>(INT_MAX) ? INT_MAX : int(n);
}

std::string quote(std::string_view s) {
    constexpr std::size_t kMax = 40;
    std::string out = "\"";
    out.append(s.substr(0, kMax));
    if (s.size() > kMax) {
        out += "...";
    }
    out.push_back('"');
    return out;
}

std::string_view stripPlus(std::string_view s) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    return s;
}

bool parseInt64Strict(std::string_view s, std::int64_t& out) {
    s = stripPlus(s);
    if (s.empty()) {
        return false;
    }
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

double parseDouble(std::string_view raw, int line, Report& report) {
    const std::string_view s = stripPlus(trim(raw));
    if (s.empty()) {
        report.warn(code::kBadNumber, "empty numeric value, using 0", line);
        return 0.0;
    }
    double d = 0.0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
    if (ec == std::errc() && p == s.data() + s.size() && std::isfinite(d)) {
        return d;
    }
    if (s.find(',') != std::string_view::npos) {
        std::string copy(s);
        std::replace(copy.begin(), copy.end(), ',', '.');
        double d2 = 0.0;
        auto [p2, ec2] = std::from_chars(copy.data(), copy.data() + copy.size(), d2);
        if (ec2 == std::errc() && p2 == copy.data() + copy.size() && std::isfinite(d2)) {
            report.warn(code::kDecimalComma, "decimal comma in " + quote(s) + ", read as a point",
                        line);
            return d2;
        }
    }
    if (ec == std::errc() && p != s.data() && std::isfinite(d)) {
        report.warn(code::kBadNumber,
                    "trailing characters after number " + quote(s) + ", using the leading part",
                    line);
        return d;
    }
    report.warn(code::kBadNumber, "not a number: " + quote(s) + ", using 0", line);
    return 0.0;
}

std::int64_t parseInteger(std::string_view raw, int line, Report& report) {
    const std::string_view s = trim(raw);
    std::int64_t v = 0;
    if (parseInt64Strict(s, v)) {
        return v;
    }
    // Some writers emit integers as "1.0" or "70.".
    const std::string_view t = stripPlus(s);
    double d = 0.0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), d);
    if (ec == std::errc() && p == t.data() + t.size() && std::isfinite(d) &&
        std::fabs(d) < 9.2e18) {
        report.warn(code::kBadNumber, "integer written as real number: " + quote(s), line);
        return static_cast<std::int64_t>(d);
    }
    report.warn(code::kBadNumber, "not an integer: " + quote(s) + ", using 0", line);
    return 0;
}

// Narrowing that keeps the bit pattern for values written as unsigned (e.g. 65535 for a
// 16-bit flag field) and clamps anything else.
std::int16_t toInt16(std::int64_t v, int line, Report& report) {
    if (v >= INT16_MIN && v <= INT16_MAX) {
        return static_cast<std::int16_t>(v);
    }
    if (v > INT16_MAX && v <= UINT16_MAX) {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(v));
    }
    report.warn(code::kBadNumber, "value " + std::to_string(v) + " does not fit 16 bits", line);
    return v < 0 ? INT16_MIN : INT16_MAX;
}

std::int32_t toInt32(std::int64_t v, int line, Report& report) {
    if (v >= INT32_MIN && v <= INT32_MAX) {
        return static_cast<std::int32_t>(v);
    }
    if (v > INT32_MAX && v <= UINT32_MAX) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
    }
    report.warn(code::kBadNumber, "value " + std::to_string(v) + " does not fit 32 bits", line);
    return v < 0 ? INT32_MIN : INT32_MAX;
}

Handle parseHandle(std::string_view raw) {
    Handle h;
    h.text = raw;
    const std::string_view s = trim(raw);
    if (s.empty() || s.size() > 16) {
        return h;
    }
    std::uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec == std::errc() && p == s.data() + s.size()) {
        h.value = v;
        h.valid = true;
    }
    return h;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::vector<std::byte> parseHexChunk(std::string_view raw, int line, Report& report) {
    const std::string_view s = trim(raw);
    std::vector<std::byte> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = hexDigit(s[i]);
        const int lo = hexDigit(s[i + 1]);
        if (hi < 0 || lo < 0) {
            report.warn(code::kBadNumber, "invalid hex digit in binary chunk", line);
            break;
        }
        out.push_back(static_cast<std::byte>(hi * 16 + lo));
    }
    if (s.size() % 2 != 0) {
        report.warn(code::kBadNumber, "odd number of hex digits in binary chunk", line);
    }
    return out;
}

Value makeAsciiValue(int groupCode, std::string_view raw, int line, Report& report) {
    switch (valueTypeForCode(groupCode)) {
        case ValueType::String:
            return groupCode == 0 ? trim(raw) : raw;
        case ValueType::Double:
            return parseDouble(raw, line, report);
        case ValueType::Int16:
            return toInt16(parseInteger(raw, line, report), line, report);
        case ValueType::Int32:
            return toInt32(parseInteger(raw, line, report), line, report);
        case ValueType::Int64:
            return parseInteger(raw, line, report);
        case ValueType::Bool:
            return parseInteger(raw, line, report) != 0;
        case ValueType::Handle:
            return parseHandle(raw);
        case ValueType::Binary:
            return parseHexChunk(raw, line, report);
    }
    return raw;
}

// Little-endian readers for binary DXF.
std::uint64_t readLe(const char* p, int n) {
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

}  // namespace

ValueType valueTypeForCode(int c) {
    if (c < 0)
        return ValueType::String;
    if (c <= 4)
        return ValueType::String;
    if (c == 5)
        return ValueType::Handle;
    if (c <= 9)
        return ValueType::String;
    if (c <= 59)
        return ValueType::Double;
    if (c <= 79)
        return ValueType::Int16;
    if (c <= 89)
        return ValueType::String;
    if (c <= 99)
        return ValueType::Int32;
    if (c == 100 || c == 102)
        return ValueType::String;
    if (c == 105)
        return ValueType::Handle;
    if (c >= 110 && c <= 149)
        return ValueType::Double;
    if (c >= 160 && c <= 169)
        return ValueType::Int64;
    if (c >= 170 && c <= 179)
        return ValueType::Int16;
    if (c >= 210 && c <= 239)
        return ValueType::Double;
    if (c >= 270 && c <= 289)
        return ValueType::Int16;
    if (c >= 290 && c <= 299)
        return ValueType::Bool;
    if (c >= 300 && c <= 309)
        return ValueType::String;
    if (c >= 310 && c <= 319)
        return ValueType::Binary;
    if (c >= 320 && c <= 369)
        return ValueType::Handle;
    if (c >= 370 && c <= 389)
        return ValueType::Int16;
    if (c >= 390 && c <= 399)
        return ValueType::Handle;
    if (c >= 400 && c <= 409)
        return ValueType::Int16;
    if (c >= 410 && c <= 419)
        return ValueType::String;
    if (c >= 420 && c <= 429)
        return ValueType::Int32;
    if (c >= 430 && c <= 439)
        return ValueType::String;
    if (c >= 440 && c <= 459)
        return ValueType::Int32;
    if (c >= 460 && c <= 469)
        return ValueType::Double;
    if (c >= 470 && c <= 479)
        return ValueType::String;
    if (c == 480 || c == 481)
        return ValueType::Handle;
    if (c == 999)
        return ValueType::String;
    if (c >= 1000 && c <= 1003)
        return ValueType::String;
    if (c == 1004)
        return ValueType::Binary;
    if (c == 1005)
        return ValueType::Handle;
    if (c >= 1010 && c <= 1059)
        return ValueType::Double;
    if (c >= 1060 && c <= 1070)
        return ValueType::Int16;
    if (c == 1071)
        return ValueType::Int32;
    return ValueType::String;
}

std::string_view Group::str() const {
    if (const auto* s = std::get_if<std::string_view>(&value))
        return *s;
    if (const auto* h = std::get_if<Handle>(&value))
        return h->text;
    return {};
}

double Group::number() const {
    if (const auto* d = std::get_if<double>(&value))
        return *d;
    if (const auto* v = std::get_if<std::int16_t>(&value))
        return *v;
    if (const auto* v = std::get_if<std::int32_t>(&value))
        return static_cast<double>(*v);
    if (const auto* v = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*v);
    if (const auto* b = std::get_if<bool>(&value))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

std::int64_t Group::integer() const {
    if (const auto* v = std::get_if<std::int16_t>(&value))
        return *v;
    if (const auto* v = std::get_if<std::int32_t>(&value))
        return *v;
    if (const auto* v = std::get_if<std::int64_t>(&value))
        return *v;
    if (const auto* b = std::get_if<bool>(&value))
        return *b ? 1 : 0;
    if (const auto* d = std::get_if<double>(&value)) {
        return std::fabs(*d) < 9.2e18 ? static_cast<std::int64_t>(*d) : 0;
    }
    return 0;
}

Result<Lexer> Lexer::open(std::span<const std::byte> bytes, Report& report) {
    Lexer lx;
    lx.report_ = &report;
    lx.data_ = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (lx.data_.empty()) {
        return makeError(ErrorCode::Parse, "empty input");
    }
    if (lx.data_.size() >= kBinarySentinel.size() &&
        lx.data_.substr(0, kBinarySentinel.size()) == kBinarySentinel) {
        lx.binary_ = true;
        lx.pos_ = kBinarySentinel.size();
        const std::string_view rest = lx.data_.substr(lx.pos_);
        // First group is "0 SECTION". R13+ stores the code in two bytes.
        if (rest.size() >= 2 + 8 && rest[0] == 0 && rest[1] == 0 &&
            rest.substr(2, 8) == std::string_view("SECTION\0", 8)) {
            lx.codeWidth_ = 2;
        } else if (rest.size() >= 1 + 8 && rest[0] == 0 &&
                   rest.substr(1, 8) == std::string_view("SECTION\0", 8)) {
            lx.codeWidth_ = 1;
        } else {
            return makeError(ErrorCode::Parse, "binary DXF: cannot determine the group code width");
        }
        report.info(
            code::kBinary,
            std::string("binary DXF with ") + std::to_string(lx.codeWidth_) + "-byte group codes",
            0);
        return lx;
    }
    if (lx.data_.size() >= 3 && lx.data_.substr(0, 3) == "\xEF\xBB\xBF") {
        lx.pos_ = 3;
        report.info(code::kBom, "UTF-8 byte order mark skipped", 1);
    }
    // The first non-blank line must be a group code.
    Lexer probe = lx;
    for (;;) {
        const auto line = probe.readLine();
        if (!line) {
            return makeError(ErrorCode::Parse, "not a DXF file: no group code found");
        }
        const std::string_view t = trim(*line);
        if (t.empty()) {
            continue;
        }
        std::int64_t v = 0;
        if (!parseInt64Strict(t, v) || v < kMinGroupCode || v > kMaxGroupCode) {
            return makeError(ErrorCode::Parse, "not a DXF file: first line is not a group code");
        }
        break;
    }
    return lx;
}

std::optional<std::string_view> Lexer::readLine() {
    if (pos_ >= data_.size()) {
        return std::nullopt;
    }
    const std::size_t start = pos_;
    std::size_t i = start;
    while (i < data_.size() && data_[i] != '\n' && data_[i] != '\r') {
        ++i;
    }
    const std::string_view line = data_.substr(start, i - start);
    if (i < data_.size()) {
        if (data_[i] == '\r' && i + 1 < data_.size() && data_[i + 1] == '\n') {
            ++i;
        }
        ++i;
    }
    pos_ = i;
    ++line_;
    return line;
}

void Lexer::finish() {
    if (done_) {
        return;
    }
    done_ = true;
    if (!sawEof_) {
        report_->warn(code::kMissingEof, "file ends without a 0/EOF marker", binary_ ? 0 : line_);
    }
}

Lexer Lexer::fromBuffer(const GroupBuffer& buffer, Report& report) {
    Lexer lx;
    lx.report_ = &report;
    lx.buffer_ = &buffer;
    return lx;
}

namespace {

void appendDouble(std::string& out, double v) {
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    const std::size_t at = out.size();
    out.append(buf, r.ptr);
    if (out.find_first_of(".eEn", at) == std::string::npos)
        out += ".0";
}

}  // namespace

std::string GroupBuffer::str() const {
    std::string out;
    out.reserve(items_.size() * 14);
    for (const Item& it : items_) {
        const std::string c = std::to_string(it.code);
        if (c.size() < 3)
            out.append(3 - c.size(), ' ');
        out += c;
        out.push_back('\n');
        switch (it.kind) {
            case Kind::Text:
                out.append(arena_, static_cast<std::size_t>(it.payload), it.len);
                break;
            case Kind::Double: {
                double v;
                std::memcpy(&v, &it.payload, 8);
                appendDouble(out, v);
                break;
            }
            case Kind::Int:
                out += std::to_string(static_cast<std::int64_t>(it.payload));
                break;
        }
        out.push_back('\n');
    }
    return out;
}

std::string_view Lexer::keep(std::string&& s) {
    return scratch_.emplace_back(std::move(s));
}

std::optional<Group> Lexer::nextBuffered() {
    const auto& items = buffer_->items_;
    if (next_ >= items.size()) {
        finish();
        return std::nullopt;
    }
    const GroupBuffer::Item& it = items[next_];
    Group g;
    g.code = it.code;
    // The line the group would have in the text: two lines per group.
    g.line = static_cast<int>(2 * next_ + 1);
    ++next_;
    const int line = g.line + 1;
    const ValueType type = valueTypeForCode(it.code);
    switch (it.kind) {
        case GroupBuffer::Kind::Double:
            if (type == ValueType::Double) {
                double v;
                std::memcpy(&v, &it.payload, 8);
                if (std::isfinite(v)) {
                    g.value = v;
                    break;
                }
            }
            {
                std::string text;
                double v;
                std::memcpy(&v, &it.payload, 8);
                appendDouble(text, v);
                g.value = makeAsciiValue(it.code, keep(std::move(text)), line, *report_);
            }
            break;
        case GroupBuffer::Kind::Int: {
            const auto v = static_cast<std::int64_t>(it.payload);
            switch (type) {
                case ValueType::Int16:
                    g.value = toInt16(v, line, *report_);
                    break;
                case ValueType::Int32:
                    g.value = toInt32(v, line, *report_);
                    break;
                case ValueType::Int64:
                    g.value = v;
                    break;
                case ValueType::Bool:
                    g.value = v != 0;
                    break;
                default:
                    g.value = makeAsciiValue(it.code, keep(std::to_string(v)), line, *report_);
                    break;
            }
            break;
        }
        case GroupBuffer::Kind::Text:
            g.value = makeAsciiValue(it.code,
                                     std::string_view(buffer_->arena_)
                                         .substr(static_cast<std::size_t>(it.payload), it.len),
                                     line, *report_);
            break;
    }
    if (it.code == 0) {
        if (const auto* s = std::get_if<std::string_view>(&g.value); s && *s == "EOF") {
            sawEof_ = true;
            done_ = true;
        }
    }
    return g;
}

std::optional<Group> Lexer::next() {
    if (done_) {
        return std::nullopt;
    }
    if (buffer_)
        return nextBuffered();
    return binary_ ? nextBinary() : nextAscii();
}

void Lexer::resyncAscii(int badLine) {
    for (;;) {
        const std::size_t posBefore = pos_;
        const int lineBefore = line_;
        const auto a = readLine();
        if (!a) {
            break;
        }
        if (trim(*a) != "0") {
            continue;
        }
        const std::size_t posAfterA = pos_;
        const int lineAfterA = line_;
        const auto b = readLine();
        if (b && isKeyword(trim(*b))) {
            pos_ = posBefore;
            line_ = lineBefore;
            report_->warn(code::kResync,
                          "skipped lines " + std::to_string(badLine) + "-" +
                              std::to_string(lineBefore - 1) + " to resynchronise at line " +
                              std::to_string(lineBefore),
                          badLine);
            return;
        }
        pos_ = posAfterA;
        line_ = lineAfterA;
    }
    report_->warn(code::kResync,
                  "no resynchronisation point after line " + std::to_string(badLine) +
                      ", rest of file ignored",
                  badLine);
    pos_ = data_.size();
    finish();
}

std::optional<Group> Lexer::nextAscii() {
    for (;;) {
        const int codeLine = line_;
        const auto codeText = readLine();
        if (!codeText) {
            finish();
            return std::nullopt;
        }
        const std::string_view ct = trim(*codeText);
        if (ct.empty()) {
            // Stray blank line where a group code is expected: skip the whole run.
            std::size_t save = pos_;
            int saveLine = line_;
            bool onlyBlanks = true;
            while (auto l = readLine()) {
                if (!trim(*l).empty()) {
                    onlyBlanks = false;
                    break;
                }
                save = pos_;
                saveLine = line_;
            }
            if (onlyBlanks) {
                finish();
                return std::nullopt;
            }
            // The non-blank line read last was consumed; step back to it.
            pos_ = save;
            line_ = saveLine;
            report_->warn(code::kBlankGroupCode, "blank line where a group code was expected",
                          codeLine);
            continue;
        }
        std::int64_t c = 0;
        if (!parseInt64Strict(ct, c) || c < kMinGroupCode || c > kMaxGroupCode) {
            report_->warn(code::kBadGroupCode, "invalid group code " + quote(ct), codeLine);
            resyncAscii(codeLine);
            if (done_) {
                return std::nullopt;
            }
            continue;
        }
        const int groupCode = static_cast<int>(c);
        const auto valueText = readLine();
        if (!valueText) {
            report_->warn(code::kMissingValue,
                          "group code " + std::to_string(groupCode) + " has no value", codeLine);
            finish();
            return std::nullopt;
        }
        Group g;
        g.code = groupCode;
        g.line = codeLine;
        g.value = makeAsciiValue(groupCode, *valueText, codeLine + 1, *report_);
        if (groupCode == 0) {
            if (const auto* s = std::get_if<std::string_view>(&g.value); s && *s == "EOF") {
                sawEof_ = true;
                done_ = true;
            }
        }
        return g;
    }
}

void Lexer::resyncBinary(std::size_t badOffset) {
    const int w = codeWidth_;
    for (std::size_t p = badOffset + 1; p + w + 1 < data_.size(); ++p) {
        if (data_[p] != 0 || (w == 2 && data_[p + 1] != 0)) {
            continue;
        }
        const std::size_t s = p + w;
        const std::size_t e = data_.find('\0', s);
        if (e == std::string_view::npos || e - s > 24) {
            continue;
        }
        if (isKeyword(data_.substr(s, e - s))) {
            pos_ = p;
            report_->warn(code::kResync,
                          "skipped bytes " + std::to_string(badOffset) + "-" +
                              std::to_string(p - 1) + " to resynchronise",
                          toLine(badOffset));
            return;
        }
    }
    report_->warn(code::kResync,
                  "no resynchronisation point after byte " + std::to_string(badOffset) +
                      ", rest of file ignored",
                  toLine(badOffset));
    pos_ = data_.size();
    finish();
}

std::optional<Group> Lexer::nextBinary() {
    for (;;) {
        if (pos_ >= data_.size()) {
            finish();
            return std::nullopt;
        }
        const std::size_t start = pos_;
        const int line = toLine(start);
        const char* base = data_.data();
        const std::size_t size = data_.size();
        std::size_t p = start;
        int groupCode = 0;
        if (codeWidth_ == 1) {
            groupCode = static_cast<unsigned char>(base[p++]);
            if (groupCode == 255) {
                if (p + 2 > size) {
                    report_->warn(code::kTruncated, "truncated group code", line);
                    finish();
                    return std::nullopt;
                }
                groupCode = static_cast<std::int16_t>(readLe(base + p, 2));
                p += 2;
            }
        } else {
            if (p + 2 > size) {
                report_->warn(code::kTruncated, "truncated group code", line);
                finish();
                return std::nullopt;
            }
            groupCode = static_cast<std::int16_t>(readLe(base + p, 2));
            p += 2;
        }
        if (groupCode < kMinGroupCode || groupCode > kMaxGroupCode) {
            report_->warn(code::kBadGroupCode, "invalid group code " + std::to_string(groupCode),
                          line);
            resyncBinary(start);
            if (done_) {
                return std::nullopt;
            }
            continue;
        }
        Group g;
        g.code = groupCode;
        g.line = line;
        const ValueType type = valueTypeForCode(groupCode);
        auto need = [&](std::size_t n) { return p + n <= size; };
        bool ok = true;
        switch (type) {
            case ValueType::String:
            case ValueType::Handle: {
                std::size_t e = data_.find('\0', p);
                bool terminated = e != std::string_view::npos;
                if (!terminated) {
                    e = size;
                    report_->warn(code::kTruncated, "unterminated string at end of file", line);
                }
                const std::string_view s = data_.substr(p, e - p);
                p = terminated ? e + 1 : e;
                if (type == ValueType::Handle) {
                    g.value = parseHandle(s);
                } else {
                    g.value = groupCode == 0 ? trim(s) : s;
                }
                break;
            }
            case ValueType::Double:
                if ((ok = need(8))) {
                    const std::uint64_t bits = readLe(base + p, 8);
                    double d;
                    std::memcpy(&d, &bits, sizeof d);
                    if (!std::isfinite(d)) {
                        report_->warn(code::kBadNumber, "non-finite real number, using 0", line);
                        d = 0.0;
                    }
                    g.value = d;
                    p += 8;
                }
                break;
            case ValueType::Int16:
                if ((ok = need(2))) {
                    g.value = static_cast<std::int16_t>(readLe(base + p, 2));
                    p += 2;
                }
                break;
            case ValueType::Int32:
                if ((ok = need(4))) {
                    g.value = static_cast<std::int32_t>(readLe(base + p, 4));
                    p += 4;
                }
                break;
            case ValueType::Int64:
                if ((ok = need(8))) {
                    g.value = static_cast<std::int64_t>(readLe(base + p, 8));
                    p += 8;
                }
                break;
            case ValueType::Bool:
                if ((ok = need(1))) {
                    g.value = base[p] != 0;
                    p += 1;
                }
                break;
            case ValueType::Binary:
                if ((ok = need(1))) {
                    const std::size_t n = static_cast<unsigned char>(base[p]);
                    ++p;
                    if ((ok = need(n))) {
                        const auto* b = reinterpret_cast<const std::byte*>(base + p);
                        g.value = std::vector<std::byte>(b, b + n);
                        p += n;
                    }
                }
                break;
        }
        if (!ok) {
            report_->warn(code::kTruncated,
                          "value of group code " + std::to_string(groupCode) +
                              " is cut off by the end of the file",
                          line);
            pos_ = size;
            finish();
            return std::nullopt;
        }
        pos_ = p;
        if (groupCode == 0) {
            if (const auto* s = std::get_if<std::string_view>(&g.value); s && *s == "EOF") {
                sawEof_ = true;
                done_ = true;
            }
        }
        return g;
    }
}

}  // namespace tbcad::dxf
