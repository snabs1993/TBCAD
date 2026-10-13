#include "tbcad/cmd/registry.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>

namespace tbcad::cmd {

namespace {

bool validName(std::string_view n) {
    if (n.empty())
        return false;
    for (char c : n) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
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

// Minimal JSON reader for one flat object of string values.
class AliasJson {
public:
    explicit AliasJson(std::string_view s) : s_(s) {}

    Result<std::map<std::string, std::string>> parse() {
        if (s_.substr(0, 3) == "\xEF\xBB\xBF")
            pos_ = 3;
        skipWs();
        if (!eat('{'))
            return fail("expected '{' at the start (aliases.json must be an object)");
        std::map<std::string, std::string> out;
        skipWs();
        if (eat('}'))
            return finish(std::move(out));
        for (;;) {
            skipWs();
            std::string key;
            if (!readString(key))
                return fail(error_.empty() ? "expected a quoted alias name" : error_);
            skipWs();
            if (!eat(':'))
                return fail("expected ':' after \"" + key + "\"");
            skipWs();
            std::string value;
            if (peek() != '"')
                return fail("the value of \"" + key + "\" must be a string");
            if (!readString(value))
                return fail(error_);
            out[key] = std::move(value);
            skipWs();
            if (eat(','))
                continue;
            if (eat('}'))
                return finish(std::move(out));
            return fail("expected ',' or '}' after the value of \"" + key + "\"");
        }
    }

private:
    Result<std::map<std::string, std::string>> finish(std::map<std::string, std::string> m) {
        skipWs();
        if (pos_ != s_.size())
            return fail("unexpected text after the closing '}'");
        return m;
    }
    Result<std::map<std::string, std::string>> fail(const std::string& msg) {
        std::size_t line = 1;
        std::size_t col = 1;
        for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return makeError(ErrorCode::Parse,
                         msg + " (line " + std::to_string(line) + ", column " +
                             std::to_string(col) + ")");
    }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool eat(char c) {
        if (peek() == c && pos_ < s_.size()) {
            ++pos_;
            return true;
        }
        return false;
    }
    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                                    s_[pos_] == '\r'))
            ++pos_;
    }
    bool hex4(std::uint32_t& v) {
        if (pos_ + 4 > s_.size())
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool readString(std::string& out) {
        error_.clear();
        if (!eat('"'))
            return false;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "control character inside a string";
                return false;
            }
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
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) {
                        error_ = "bad \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 1 < s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        std::uint32_t lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            error_ = "bad surrogate pair";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    error_ = "unknown escape sequence";
                    return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_{};
};

}  // namespace

Result<std::map<std::string, std::string>> parseAliasJson(std::string_view text) {
    return AliasJson(text).parse();
}

Registry::Registry() = default;
Registry::~Registry() = default;

Result<void> Registry::add(std::unique_ptr<Command> command) {
    if (!command)
        return makeError(ErrorCode::InvalidArgument, "null command");
    std::string name(command->name());
    if (!validName(name))
        return makeError(ErrorCode::InvalidArgument,
                         "command name \"" + name + "\" must be upper-case ASCII (A-Z 0-9 _ -)");
    if (commands_.count(name))
        return makeError(ErrorCode::Conflict, "command \"" + name + "\" is already registered");
    commands_.emplace(std::move(name), std::move(command));
    rebuild();
    return {};
}

Command* Registry::find(std::string_view nameOrAlias) const {
    std::string key = toUpperAscii(trim(nameOrAlias));
    if (auto it = commands_.find(key); it != commands_.end())
        return it->second.get();
    if (auto a = aliases_.find(key); a != aliases_.end()) {
        if (auto it = commands_.find(a->second); it != commands_.end())
            return it->second.get();
    }
    return nullptr;
}

std::vector<Command*> Registry::list() const {
    std::vector<Command*> out;
    out.reserve(commands_.size());
    for (const auto& [name, c] : commands_)
        out.push_back(c.get());
    return out;
}

std::vector<std::string> Registry::aliasesOf(std::string_view commandName) const {
    std::string key = toUpperAscii(commandName);
    std::vector<std::string> out;
    for (const auto& [alias, target] : aliases_) {
        if (target == key)
            out.push_back(alias);
    }
    return out;
}

AliasReport Registry::rebuild() {
    aliases_.clear();
    conflicts_.clear();
    AliasReport report;

    std::map<std::string, std::string> owner;  // alias -> first claimant
    std::set<std::string> ambiguous;
    for (const auto& [name, cmd] : commands_) {
        for (const std::string& raw : cmd->defaultAliases()) {
            std::string a = toUpperAscii(raw);
            if (!validName(a)) {
                conflicts_.push_back("Command " + name + ": invalid alias \"" + raw + "\" ignored");
                continue;
            }
            if (commands_.count(a)) {
                conflicts_.push_back("Alias " + a + " of " + name +
                                     " ignored: it is the name of another command");
                continue;
            }
            auto [it, inserted] = owner.emplace(a, name);
            if (!inserted && it->second != name) {
                if (ambiguous.insert(a).second)
                    conflicts_.push_back("Alias " + a + " is claimed by both " + it->second +
                                         " and " + name + "; neither gets it until a user alias "
                                         "decides");
                else
                    conflicts_.back() += " (also " + name + ")";
            }
        }
    }
    for (const auto& [a, name] : owner) {
        if (!ambiguous.count(a))
            aliases_[a] = name;
    }

    for (const auto& [rawAlias, rawTarget] : user_) {
        std::string a = toUpperAscii(trim(rawAlias));
        std::string t = toUpperAscii(trim(rawTarget));
        if (!validName(a)) {
            report.errors.push_back("aliases.json: \"" + rawAlias + "\" is not a valid alias");
            continue;
        }
        if (commands_.count(a)) {
            report.errors.push_back("aliases.json: \"" + rawAlias +
                                    "\" is the name of a command and cannot be an alias");
            continue;
        }
        if (!commands_.count(t)) {
            report.errors.push_back("aliases.json: \"" + rawAlias + "\" points to unknown command \"" +
                                    rawTarget + "\"");
            continue;
        }
        aliases_[a] = t;
        ++report.userAliases;
    }
    report.warnings = conflicts_;
    return report;
}

AliasReport Registry::setUserAliases(const std::map<std::string, std::string>& userAliases) {
    user_ = userAliases;
    AliasReport r = rebuild();
    r.userFileLoaded = true;
    return r;
}

AliasReport Registry::loadUserAliases(const std::filesystem::path& file) {
    user_.clear();
    if (file.empty()) {
        return rebuild();
    }
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return rebuild();  // no user file: defaults only
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        AliasReport r = rebuild();
        r.errors.push_back("cannot read " + file.string());
        return r;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = parseAliasJson(buf.str());
    if (!parsed) {
        AliasReport r = rebuild();
        r.errors.insert(r.errors.begin(), file.filename().string() + ": " + parsed.error().message +
                                              "; using the default aliases");
        return r;
    }
    return setUserAliases(parsed.value());
}

}  // namespace tbcad::cmd
