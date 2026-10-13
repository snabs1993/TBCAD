// A very small JSON reader for the .expected.json files of the corpus (tests only).
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tbcad::dxf::testing {

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    bool has(const std::string& key) const { return kind == Kind::Object && o.count(key) > 0; }
    const Json& at(const std::string& key) const { return o.at(key); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : t_(text) {}
    Json parse() {
        Json v = value();
        ws();
        if (i_ != t_.size())
            throw std::runtime_error("trailing characters in JSON");
        return v;
    }

private:
    void ws() {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_])))
            ++i_;
    }
    char peek() {
        ws();
        if (i_ >= t_.size())
            throw std::runtime_error("unexpected end of JSON");
        return t_[i_];
    }
    void expectChar(char c) {
        if (peek() != c)
            throw std::runtime_error(std::string("expected '") + c + "' in JSON");
        ++i_;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
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
    std::string str() {
        expectChar('"');
        std::string out;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            char e = t_.at(i_++);
            switch (e) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    unsigned cp = static_cast<unsigned>(std::stoul(t_.substr(i_, 4), nullptr, 16));
                    i_ += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {
                        unsigned lo =
                            static_cast<unsigned>(std::stoul(t_.substr(i_ + 2, 4), nullptr, 16));
                        i_ += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    out.push_back(e);
            }
        }
        expectChar('"');
        return out;
    }
    Json value() {
        Json v;
        char c = peek();
        if (c == '{') {
            ++i_;
            v.kind = Json::Kind::Object;
            if (peek() == '}') {
                ++i_;
                return v;
            }
            while (true) {
                std::string key = str();
                expectChar(':');
                v.o[key] = value();
                if (peek() == ',') {
                    ++i_;
                    continue;
                }
                expectChar('}');
                break;
            }
        } else if (c == '[') {
            ++i_;
            v.kind = Json::Kind::Array;
            if (peek() == ']') {
                ++i_;
                return v;
            }
            while (true) {
                v.a.push_back(value());
                if (peek() == ',') {
                    ++i_;
                    continue;
                }
                expectChar(']');
                break;
            }
        } else if (c == '"') {
            v.kind = Json::Kind::String;
            v.s = str();
        } else if (t_.compare(i_, 4, "true") == 0) {
            v.kind = Json::Kind::Bool;
            v.b = true;
            i_ += 4;
        } else if (t_.compare(i_, 5, "false") == 0) {
            v.kind = Json::Kind::Bool;
            i_ += 5;
        } else if (t_.compare(i_, 4, "null") == 0) {
            i_ += 4;
        } else {
            v.kind = Json::Kind::Number;
            char* end = nullptr;
            v.n = std::strtod(t_.c_str() + i_, &end);
            if (end == t_.c_str() + i_)
                throw std::runtime_error("bad JSON value");
            i_ = static_cast<std::size_t>(end - t_.c_str());
        }
        return v;
    }

    const std::string& t_;
    std::size_t i_ = 0;
};

}  // namespace tbcad::dxf::testing
