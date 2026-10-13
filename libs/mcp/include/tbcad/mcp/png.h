#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tbcad::mcp {

// PNG (8-bit RGBA) of the pixels; empty on failure. Uses miniz.
std::vector<std::uint8_t> encodePng(const std::vector<std::uint8_t>& rgba, int width, int height);

std::string base64Encode(const std::vector<std::uint8_t>& bytes);

}  // namespace tbcad::mcp
