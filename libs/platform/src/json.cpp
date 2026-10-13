#include "tbcad/platform/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tbcad::platform {

namespace {

const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<Json> parseDocument() {
        skipSpace();
        auto v = parseValue(0);
        if (!v)
            return v;
        skipSpace();
        if (pos_ != s_.size())
            return fail("unexpected text after the value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    Error fail(const std::string& what) const {
        return makeError(ErrorCode::Parse, "JSON error at offset " + std::to_string(pos_) + ": " + what);
    }
    void skipSpace() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }
    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool consumeWord(std::string_view w) {
        if (s_.substr(pos_, w.size()) == w) {
            pos_ += w.size();
            return true;
        }
        return false;
    }

    Result<Json> parseValue(int depth) {
        if (depth > kMaxDepth)
            return fail("nesting too deep");
        if (pos_ >= s_.size())
            return fail("unexpected end of text");
        char c = s_[pos_];
        if (c == '{')
            return parseObject(depth);
        if (c == '[')
            return parseArray(depth);
        if (c == '"') {
            auto s = parseString();
            if (!s)
                return s.error();
            return Json(std::move(s.value()));
        }
        if (consumeWord("true"))
            return Json(true);
        if (consumeWord("false"))
            return Json(false);
        if (consumeWord("null"))
            return Json(nullptr);
        return parseNumber();
    }

    Result<Json> parseNumber() {
        std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-')
            ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' ||
                                    s_[pos_] == '-'))
            ++pos_;
        if (pos_ == start)
            return fail("unexpected character");
        std::string tok(s_.substr(start, pos_ - start));
        char* end = nullptr;
        double d = std::strtod(tok.c_str(), &end);
        if (end != tok.c_str() + tok.size() || !std::isfinite(d))
            return fail("bad number '" + tok + "'");
        return Json(d);
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size())
            return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        pos_ += 4;
        out = v;
        return true;
    }

    Result<std::string> parseString() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size())
                break;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp))
                        return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        unsigned lo = 0;
                        if (!(consumeWord("\\u") && hex4(lo)) || lo < 0xDC00 || lo > 0xDFFF)
                            return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    Result<Json> parseArray(int depth) {
        ++pos_;
        Json::Array arr;
        skipSpace();
        if (consume(']'))
            return Json(std::move(arr));
        for (;;) {
            skipSpace();
            auto v = parseValue(depth + 1);
            if (!v)
                return v;
            arr.push_back(std::move(v.value()));
            skipSpace();
            if (consume(','))
                continue;
            if (consume(']'))
                return Json(std::move(arr));
            return fail("expected ',' or ']'");
        }
    }

    Result<Json> parseObject(int depth) {
        ++pos_;
        Json::Object obj;
        skipSpace();
        if (consume('}'))
            return Json(std::move(obj));
        for (;;) {
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"')
                return fail("expected a string key");
            auto key = parseString();
            if (!key)
                return key.error();
            skipSpace();
            if (!consume(':'))
                return fail("expected ':'");
            skipSpace();
            auto v = parseValue(depth + 1);
            if (!v)
                return v;
            obj[std::move(key.value())] = std::move(v.value());
            skipSpace();
            if (consume(','))
                continue;
            if (consume('}'))
                return Json(std::move(obj));
            return fail("expected ',' or '}'");
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[40];
    if (d == std::floor(d) && std::fabs(d) < 1e15)
        std::snprintf(buf, sizeof buf, "%.0f", d);
    else
        std::snprintf(buf, sizeof buf, "%.17g", d);
    out += buf;
}

void write(std::string& out, const Json& v, bool pretty, int indent) {
    auto nl = [&](int level) {
        if (!pretty)
            return;
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    if (v.isNull()) {
        out += "null";
    } else if (v.isBool()) {
        out += v.asBool() ? "true" : "false";
    } else if (v.isNumber()) {
        writeNumber(out, v.asNumber());
    } else if (v.isString()) {
        writeString(out, v.asString());
    } else if (v.isArray()) {
        const auto& a = v.asArray();
        if (a.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i)
                out += ',';
            nl(indent + 1);
            write(out, a[i], pretty, indent + 1);
        }
        nl(indent);
        out += ']';
    } else {
        const auto& o = v.asObject();
        if (o.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const auto& [k, val] : o) {
            if (!first)
                out += ',';
            first = false;
            nl(indent + 1);
            writeString(out, k);
            out += pretty ? ": " : ":";
            write(out, val, pretty, indent + 1);
        }
        nl(indent);
        out += '}';
    }
}

}  // namespace

bool Json::asBool(bool fallback) const {
    const bool* b = std::get_if<bool>(&v_);
    return b ? *b : fallback;
}
double Json::asNumber(double fallback) const {
    const double* d = std::get_if<double>(&v_);
    return d ? *d : fallback;
}
const std::string& Json::asString() const {
    const auto* s = std::get_if<std::string>(&v_);
    return s ? *s : kEmptyString;
}
const Json::Array& Json::asArray() const {
    const auto* a = std::get_if<Array>(&v_);
    return a ? *a : kEmptyArray;
}
const Json::Object& Json::asObject() const {
    const auto* o = std::get_if<Object>(&v_);
    return o ? *o : kEmptyObject;
}
Json::Array& Json::array() {
    if (!isArray())
        v_ = Array{};
    return std::get<Array>(v_);
}
Json::Object& Json::object() {
    if (!isObject())
        v_ = Object{};
    return std::get<Object>(v_);
}
const Json* Json::find(const std::string& key) const {
    const auto* o = std::get_if<Object>(&v_);
    if (!o)
        return nullptr;
    auto it = o->find(key);
    return it == o->end() ? nullptr : &it->second;
}

Result<Json> parseJson(std::string_view text) {
    // Skip a UTF-8 byte order mark: Notepad writes one.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    return Parser(text).parseDocument();
}

std::string toJson(const Json& value, bool pretty) {
    std::string out;
    write(out, value, pretty, 0);
    if (pretty)
        out += '\n';
    return out;
}

}  // namespace tbcad::platform
