#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "map/dxf_text.h"
#include "tbcad/dwg/dwg.h"

using namespace tbcad;

static std::vector<std::byte> bytesOf(const std::string& s) {
    std::vector<std::byte> v;
    for (char c : s)
        v.push_back(static_cast<std::byte>(c));
    return v;
}

TEST_CASE("version signatures") {
    auto info = [](const char* s) { return dwg::detectVersion(bytesOf(std::string(s) + "\0\0\0\0\0")); };
    CHECK(info("AC1015").version == dwg::Version::R2000);
    CHECK(info("AC1018").version == dwg::Version::R2004);
    CHECK(info("AC1021").version == dwg::Version::R2007);
    CHECK(info("AC1024").version == dwg::Version::R2010);
    CHECK(info("AC1027").version == dwg::Version::R2013);
    CHECK(info("AC1032").version == dwg::Version::R2018);
    CHECK(info("AC1032").supported);
    CHECK(info("AC1014").version == dwg::Version::R14);
    CHECK_FALSE(info("AC1014").supported);
    CHECK(info("AC1009").version == dwg::Version::R12OrOlder);
    CHECK(info("AC1099").version == dwg::Version::Unknown);
    CHECK(dwg::detectVersion(bytesOf("hello world")).code.empty());
    CHECK(dwg::detectVersion(bytesOf("AC10")).code.empty());
}

TEST_CASE("importDwg rejects non-DWG and unsupported versions") {
    dxf::Report report;
    auto r = dwg::importDwg(bytesOf("SECTION\n  0\n"), {}, report);
    CHECK_FALSE(r);
    auto r14 = dwg::importDwg(bytesOf("AC1014xxxxxxxxxxx"), {}, report);
    CHECK_FALSE(r14);
    CHECK(report.countCode(dwg::code::kUnsupportedVersion) == 1);
}

TEST_CASE("DxfText formatting") {
    dwg::detail::DxfText t;
    t.begin("LINE");
    t.handle(5, 0x1Fu);
    t.group(10, 1.5);
    t.group(20, 2.0);
    t.group(62, 3);
    t.group(1, std::string("a\nb"));
    CHECK(t.str() == "  0\nLINE\n  5\n1F\n 10\n1.5\n 20\n2.0\n 62\n3\n  1\na b\n");
}
