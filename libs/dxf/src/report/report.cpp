#include "tbcad/dxf/report.h"

#include <cstdio>

namespace tbcad::dxf {

namespace {

constexpr const char* severityName(Severity s) {
    switch (s) {
        case Severity::Info:
            return "info";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }
    return "warning";
}

// Appends `s` as a JSON string body. Bytes that are not valid UTF-8 become U+FFFD so the
// output is always valid JSON even when a message quotes raw file content.
void appendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
            ++i;
            continue;
        }
        std::size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        }
        bool valid = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
            valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        }
        if (valid && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            valid = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);
        } else if (valid && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            valid = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        if (valid) {
            out.append(s.substr(i, len));
            i += len;
        } else {
            out += "\\ufffd";
            ++i;
        }
    }
    out.push_back('"');
}

}  // namespace

void Report::add(Severity severity, std::string_view code, std::string message, int line,
                 std::optional<std::uint64_t> handle) {
    ++counts_[static_cast<int>(severity)];
    if (items_.size() == kMaxDiagnostics) {
        items_.push_back({Severity::Warning, std::string(code::kTooManyDiagnostics),
                          "further diagnostics were counted but not recorded", 0, std::nullopt});
    }
    if (items_.size() > kMaxDiagnostics) {
        return;
    }
    items_.push_back({severity, std::string(code), std::move(message), line, handle});
}

std::size_t Report::countCode(std::string_view code) const {
    std::size_t n = 0;
    for (const Diagnostic& d : items_) {
        if (d.code == code) {
            ++n;
        }
    }
    return n;
}

std::string Report::toJson() const {
    std::string out = "{\"info\":" + std::to_string(count(Severity::Info)) +
                      ",\"warnings\":" + std::to_string(count(Severity::Warning)) +
                      ",\"errors\":" + std::to_string(count(Severity::Error)) +
                      ",\"diagnostics\":[";
    bool first = true;
    for (const Diagnostic& d : items_) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += "{\"severity\":\"";
        out += severityName(d.severity);
        out += "\",\"code\":";
        appendJsonString(out, d.code);
        out += ",\"message\":";
        appendJsonString(out, d.message);
        out += ",\"line\":" + std::to_string(d.line);
        if (d.handle) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(*d.handle));
            out += ",\"handle\":\"";
            out += buf;
            out.push_back('"');
        }
        out.push_back('}');
    }
    out += "]}";
    return out;
}

}  // namespace tbcad::dxf
