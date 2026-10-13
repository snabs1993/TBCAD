#pragma once

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace tbcad::dxf::testing {

inline std::vector<std::byte> bytesOf(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

inline std::vector<std::byte> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto* p = reinterpret_cast<const std::byte*>(raw.data());
    return std::vector<std::byte>(p, p + raw.size());
}

inline std::string corpusPath(const std::string& relative) {
    return std::string(TBCAD_TEST_CORPUS_DIR) + "/" + relative;
}

}  // namespace tbcad::dxf::testing
