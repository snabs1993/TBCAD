// Every file in tests/corpus/fuzz/dwg (seeds and `repro-*` inputs that once crashed the reader)
// opens or is refused without a crash; run with the sanitizers to catch out-of-bounds reads.
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "tbcad/dwg/dwg.h"

TEST_CASE("the DWG fuzz corpus opens or is refused without a crash") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(TBCAD_TEST_CORPUS_DIR) / "fuzz" / "dwg";
    REQUIRE(fs::is_directory(dir));
    int files = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".dwg")
            continue;
        ++files;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
            bytes[i] = static_cast<std::byte>(raw[i]);
        tbcad::dxf::Report report;
        auto r = tbcad::dwg::importDwg(bytes, tbcad::dxf::ImportOptions{}, report);
        CAPTURE(entry.path().filename().string());
        if (!r.ok())
            CHECK_FALSE(r.error().message.empty());
    }
    CHECK(files >= 3);
}
