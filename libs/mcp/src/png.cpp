#include "tbcad/mcp/png.h"

#include <miniz.h>

namespace tbcad::mcp {

std::vector<std::uint8_t> encodePng(const std::vector<std::uint8_t>& rgba, int width, int height) {
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        return {};
    std::size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), width, height, 4, &size, 6,
                                                           MZ_FALSE);
    if (!png)
        return {};
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(png),
                                  static_cast<const std::uint8_t*>(png) + size);
    mz_free(png);
    return out;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) |
                          bytes[i + 2];
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i + 1 == bytes.size()) {
        std::uint32_t v = std::uint32_t{bytes[i]} << 16;
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

}  // namespace tbcad::mcp
