#include "tbcad/platform/clipboard_picture.h"

#include <cstring>

namespace tbcad::platform {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

}  // namespace

std::optional<ClipboardPicture> parseDib(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 40)
        return std::nullopt;
    const std::uint32_t headerSize = le32(data);
    if (headerSize < 40 || headerSize > size)
        return std::nullopt;
    const std::int64_t w = static_cast<std::int32_t>(le32(data + 4));
    std::int64_t h = static_cast<std::int32_t>(le32(data + 8));
    const bool topDown = h < 0;
    if (topDown)
        h = -h;
    const unsigned bits = le16(data + 14);
    const std::uint32_t compression = le32(data + 16);
    std::uint32_t colours = le32(data + 32);
    if (w <= 0 || h <= 0 || w > 30000 || h > 30000)
        return std::nullopt;
    if (compression != 0 && compression != 3)  // BI_RGB, BI_BITFIELDS; no RLE or JPEG
        return std::nullopt;
    if (bits != 8 && bits != 24 && bits != 32)
        return std::nullopt;

    std::size_t offset = headerSize;
    // BI_BITFIELDS with a plain 40-byte header stores three masks after it; the pixels are read as
    // BGR(A), which is what every clipboard writer produces.
    if (compression == 3 && headerSize == 40)
        offset += 12;
    std::vector<std::uint32_t> palette;
    if (bits == 8) {
        if (colours == 0 || colours > 256)
            colours = 256;
        if (offset + std::size_t{colours} * 4 > size)
            return std::nullopt;
        for (std::uint32_t i = 0; i < colours; ++i)
            palette.push_back(le32(data + offset + i * 4));
        offset += std::size_t{colours} * 4;
    }
    const std::size_t stride = ((static_cast<std::size_t>(w) * bits + 31) / 32) * 4;
    if (offset > size || stride * static_cast<std::size_t>(h) > size - offset)
        return std::nullopt;

    ClipboardPicture pic;
    pic.width = static_cast<int>(w);
    pic.height = static_cast<int>(h);
    pic.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
    bool anyAlpha = false;
    for (std::int64_t y = 0; y < h; ++y) {
        const std::int64_t srcRow = topDown ? y : h - 1 - y;
        const std::uint8_t* src = data + offset + stride * static_cast<std::size_t>(srcRow);
        std::uint8_t* dst = pic.rgba.data() + static_cast<std::size_t>(y * w) * 4;
        for (std::int64_t x = 0; x < w; ++x, dst += 4) {
            if (bits == 8) {
                const std::uint32_t c = src[x] < palette.size() ? palette[src[x]] : 0;
                dst[0] = static_cast<std::uint8_t>(c >> 16);
                dst[1] = static_cast<std::uint8_t>(c >> 8);
                dst[2] = static_cast<std::uint8_t>(c);
            } else {
                const std::uint8_t* px = src + static_cast<std::size_t>(x) * (bits / 8);
                dst[0] = px[2];
                dst[1] = px[1];
                dst[2] = px[0];
                if (bits == 32) {
                    dst[3] = px[3];
                    anyAlpha = anyAlpha || px[3] != 0;
                }
            }
        }
    }
    // Most programs leave the fourth byte of a 32-bit clipboard bitmap at zero: opaque then.
    if (bits == 32 && !anyAlpha)
        for (std::size_t i = 3; i < pic.rgba.size(); i += 4)
            pic.rgba[i] = 255;
    return pic;
}

}  // namespace tbcad::platform
