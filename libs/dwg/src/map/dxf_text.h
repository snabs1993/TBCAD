// Internal: collects the groups decoded from a DWG file, in the order and with the group codes
// of a DXF file (the public DXF Reference), for the DXF importer. They stay in memory as a
// dxf::GroupBuffer; str() gives the same groups as DXF text.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tbcad/dxf/lexer.h"

namespace tbcad::dwg::detail {

class DxfText {
public:
    void group(int code, std::string_view s) { buf_.add(code, s); }
    void group(int code, const char* s) { group(code, std::string_view(s)); }
    void group(int code, const std::string& s) { group(code, std::string_view(s)); }
    void group(int code, int v) { buf_.addInt(code, v); }
    void group(int code, long long v) { buf_.addInt(code, v); }
    void group(int code, std::uint64_t v) { buf_.addInt(code, static_cast<std::int64_t>(v)); }
    void group(int code, double v) { buf_.add(code, v); }
    // Handle groups (5, 105, 330, ...): uppercase hexadecimal.
    void handle(int code, std::uint64_t h) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        char buf[17];
        int n = 0;
        do {
            buf[n++] = kHex[h & 15];
            h >>= 4;
        } while (h);
        std::string s;
        while (n)
            s.push_back(buf[--n]);
        buf_.add(code, s);
    }

    void begin(std::string_view type) { group(0, type); }
    void beginSection(std::string_view name) {
        begin("SECTION");
        group(2, name);
    }
    void endSection() { begin("ENDSEC"); }
    void endFile() { begin("EOF"); }

    const dxf::GroupBuffer& buffer() const { return buf_; }
    dxf::GroupBuffer takeBuffer() { return std::move(buf_); }
    std::string str() const { return buf_.str(); }

private:
    dxf::GroupBuffer buf_{};
};

}  // namespace tbcad::dwg::detail
