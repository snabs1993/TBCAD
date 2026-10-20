#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "tbcad/io/zip.h"

using namespace tbcad;

TEST_CASE("zip: entries round trip, compressible ones deflated, names kept as UTF-8") {
    std::vector<io::ZipEntry> in(4);
    in[0].name = "plan.tbcad";
    in[0].data.assign(5000, 'a');  // packs well: deflated
    in[1].name = "refs/site plan.tbcad";
    for (int i = 0; i < 300; ++i)
        in[1].data.push_back(static_cast<std::uint8_t>((i * 7919) % 251));
    in[2].name = "fonts/\xE0\xB8\x97\xE0\xB8\xB5.shx";  // Thai name
    in[3].name = "empty.txt";
    auto bytes = io::zipBytes(in);
    REQUIRE(bytes.ok());
    CHECK(bytes.value().size() < 5000);
    auto out = io::readZip(bytes.value());
    REQUIRE(out.ok());
    REQUIRE(out.value().size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(out.value()[i].name == in[i].name);
        CHECK(out.value()[i].data == in[i].data);
    }
}

TEST_CASE("zip: a damaged archive is refused") {
    std::vector<io::ZipEntry> in(1);
    in[0].name = "a.txt";
    in[0].data.assign(100, 'x');
    auto bytes = io::zipBytes(in).value();
    auto broken = bytes;
    broken[36] ^= 0xFF;  // inside the packed data
    CHECK_FALSE(io::readZip(broken).ok());
    CHECK_FALSE(io::readZip(std::span<const std::uint8_t>(bytes.data(), 10)).ok());
}

TEST_CASE("zip: writeZip writes the file") {
    const auto file = std::filesystem::temp_directory_path() / "tbcad_zip_test.zip";
    std::vector<io::ZipEntry> in(1);
    in[0].name = "transmittal.txt";
    const std::string text = "Transmittal of plan.tbcad\n";
    in[0].data.assign(text.begin(), text.end());
    REQUIRE(io::writeZip(file, in).ok());
    std::ifstream f(file, std::ios::binary);
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
    auto out = io::readZip(raw);
    REQUIRE(out.ok());
    CHECK(out.value()[0].data == in[0].data);
    f.close();
    std::filesystem::remove(file);
}
