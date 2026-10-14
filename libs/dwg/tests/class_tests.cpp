#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "container/sections.h"

namespace tbcad::dwg::test {
namespace {

// The few bit codes the class section needs.
class BitWriter {
public:
    void bits(std::uint64_t v, unsigned n) {
        for (unsigned i = n; i-- > 0;) {
            if (pos_ % 8 == 0)
                bytes_.push_back(0);
            if ((v >> i) & 1)
                bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (0x80 >> (pos_ % 8)));
            ++pos_;
        }
    }
    void b(bool v) { bits(v ? 1 : 0, 1); }
    void rc(unsigned v) { bits(v & 0xFF, 8); }
    void bs(int v) {  // always the long form: code 00 and two bytes
        bits(0, 2);
        rc(static_cast<unsigned>(v));
        rc(static_cast<unsigned>(v) >> 8);
    }
    void bl(int v) {
        bits(0, 2);
        for (int i = 0; i < 4; ++i)
            rc(static_cast<unsigned>(v) >> (8 * i));
    }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_{};
    std::size_t pos_ = 0;
};

void text(BitWriter& w, const std::string& s) {
    w.bs(static_cast<int>(s.size()));
    for (char c : s) {
        w.rc(static_cast<unsigned char>(c));
        w.rc(0);
    }
}

}  // namespace

// A class section as a real R2018 file writes it: size, high size word, bit count, then all class
// fields, then one run of UTF-16 names (application, C++ name, DXF name per class).
TEST_CASE("dwg: R2018 class section with its names after the fields") {
    BitWriter w;
    w.bs(501);  // maximum class number
    w.rc(0);
    w.rc(0);
    w.b(true);
    for (int i = 0; i < 2; ++i) {
        w.bs(500 + i);
        w.bs(0);
        w.b(false);
        w.bs(i == 0 ? 0x1F3 : 0x1F2);
        w.bl(3);
        w.bs(22);
        w.bs(0);
        w.bl(0);
        w.bl(0);
    }
    text(w, "ObjectDBX Classes");
    text(w, "AcDbLayout");
    text(w, "LAYOUT");
    text(w, "ObjectDBX Classes");
    text(w, "AcDbHatch");
    text(w, "HATCH");
    const std::vector<std::uint8_t> data = w.bytes();
    std::vector<std::uint8_t> section = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
                                         0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A};
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            section.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put32(static_cast<std::uint32_t>(data.size()));
    put32(0);
    put32(static_cast<std::uint32_t>(data.size() * 8));
    section.insert(section.end(), data.begin(), data.end());
    detail::Format fmt;
    fmt.r2004 = fmt.r2007 = fmt.r2010 = fmt.r2013 = fmt.r2018 = true;
    std::vector<std::string> notes;
    const auto classes = detail::parseClasses(section, fmt, 4, notes);
    REQUIRE(classes.size() == 2);
    CHECK(classes[0].number == 500);
    CHECK(classes[0].dxfName == "LAYOUT");
    CHECK_FALSE(classes[0].entity);
    CHECK(classes[1].dxfName == "HATCH");
    CHECK(classes[1].cppName == "AcDbHatch");
    CHECK(classes[1].entity);
}

}  // namespace tbcad::dwg::test
