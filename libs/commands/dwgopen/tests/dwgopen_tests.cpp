#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/core/core.h"
#include "tbcad/dwgopen/dwgopen.h"

using namespace tbcad;
namespace fs = std::filesystem;

TEST_CASE("readDwgFile rejects missing, junk and unsupported files without touching them") {
    fs::path dir = fs::temp_directory_path() / "tbcad_dwgopen_tests";
    fs::create_directories(dir);
    CHECK_FALSE(dwgopen::readDwgFile(dir / "missing.dwg").ok());
    { std::ofstream(dir / "junk.dwg", std::ios::binary) << "not a drawing at all"; }
    CHECK_FALSE(dwgopen::readDwgFile(dir / "junk.dwg").ok());
    { std::ofstream(dir / "old.dwg", std::ios::binary) << "AC1014 old release"; }
    auto r = dwgopen::readDwgFile(dir / "old.dwg");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("unsupported") != std::string::npos);
    fs::remove_all(dir);
}
