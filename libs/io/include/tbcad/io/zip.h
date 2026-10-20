#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/result.h"

namespace tbcad::io {

// A plain .zip archive (deflate or stored entries, UTF-8 names, no ZIP64), as ETRANSMIT writes it
// for sending a drawing with its references. Names use '/' between folders.
struct ZipEntry {
    std::string name{};
    std::vector<std::uint8_t> data{};
};

// The archive bytes. Entries that do not get smaller are stored. Fails past 4 GB or 65535
// entries.
Result<std::vector<std::uint8_t>> zipBytes(const std::vector<ZipEntry>& entries);
Result<void> writeZip(const std::filesystem::path& file, const std::vector<ZipEntry>& entries);
// Reads an archive written by zipBytes (or any zip with stored / deflated entries, no ZIP64);
// checks every entry's CRC.
Result<std::vector<ZipEntry>> readZip(std::span<const std::uint8_t> bytes);

}  // namespace tbcad::io
