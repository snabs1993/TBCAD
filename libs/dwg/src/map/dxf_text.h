// Internal: builds the in-memory ASCII DXF text that carries the records decoded from a DWG
// file into the DXF importer. Group codes and meanings follow the public DXF Reference.
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace tbcad::dwg::detail {

class DxfText {
public:
    void group(int code, std::string_view s) {
        line(code);
        // A DXF text value is one line: control characters would split it.
        for (char c : s)
            out_.push_back(c == '\n' || c == '\r' ? ' ' : c);
        out_.push_back('\n');
    }
    void group(int code, const char* s) { group(code, std::string_view(s)); }
    void group(int code, const std::string& s) { group(code, std::string_view(s)); }
    void group(int code, int v) { group(code, static_cast<long long>(v)); }
    void group(int code, long long v) {
        line(code);
        out_ += std::to_string(v);
        out_.push_back('\n');
    }
    void group(int code, std::uint64_t v) {
        line(code);
        out_ += std::to_string(v);
        out_.push_back('\n');
    }
    void group(int code, double v) {
        line(code);
        char buf[40];
        auto r = std::to_chars(buf, buf + sizeof buf, v);
        out_.append(buf, r.ptr);
        if (out_.find_first_of(".eEn", out_.size() - static_cast<std::size_t>(r.ptr - buf)) ==
            std::string::npos)
            out_ += ".0";
        out_.push_back('\n');
    }
    // Handle groups (5, 105, 330, ...): uppercase hexadecimal.
    void handle(int code, std::uint64_t h) {
        line(code);
        static constexpr char kHex[] = "0123456789ABCDEF";
        char buf[17];
        int n = 0;
        do {
            buf[n++] = kHex[h & 15];
            h >>= 4;
        } while (h);
        while (n)
            out_.push_back(buf[--n]);
        out_.push_back('\n');
    }

    void begin(std::string_view type) { group(0, type); }
    void beginSection(std::string_view name) {
        begin("SECTION");
        group(2, name);
    }
    void endSection() { begin("ENDSEC"); }
    void endFile() { begin("EOF"); }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void line(int code) {
        std::string c = std::to_string(code);
        if (c.size() < 3)
            out_.append(3 - c.size(), ' ');
        out_ += c;
        out_.push_back('\n');
    }
    std::string out_{};
};

}  // namespace tbcad::dwg::detail
