#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tbcad::platform {

// A picture taken from the system clipboard: width * height pixels, top row first, RGBA.
struct ClipboardPicture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Reads a Windows device-independent bitmap (the CF_DIB clipboard format: a BITMAPINFOHEADER, an
// optional colour table or bit masks, then the pixels). Understands 8, 24 and 32 bits per pixel,
// bottom-up and top-down rows. Null for anything else or for data that is cut short.
std::optional<ClipboardPicture> parseDib(const std::uint8_t* data, std::size_t size);

// The picture on the system clipboard, if there is one. Null on platforms without a clipboard
// picture reader.
std::optional<ClipboardPicture> readClipboardPicture();

// The inverse of parseDib: a 24-bit bottom-up DIB (BITMAPINFOHEADER and pixels, no file header).
// Alpha is dropped; transparent pixels come out white.
std::vector<std::uint8_t> buildDib(const ClipboardPicture& pic);

// A picture a command wants on the system clipboard. COPYCLIP leaves one here; the window takes it
// after the command and calls writeClipboard. One slot, used from the main thread only.
void stashClipboardPicture(ClipboardPicture pic);
std::optional<ClipboardPicture> takeStashedClipboardPicture();

// Puts UTF-8 text and an optional picture on the system clipboard together, replacing what was
// there. False on platforms without a clipboard picture writer or when the clipboard is busy.
bool writeClipboard(const std::string& utf8Text, const ClipboardPicture* pic);

}  // namespace tbcad::platform
