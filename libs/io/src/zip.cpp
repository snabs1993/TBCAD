#include "tbcad/io/zip.h"

#include <miniz.h>

#include <ctime>
#include <fstream>
#include <limits>

namespace tbcad::io {

namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint16_t kUtf8Flag = 0x0800;

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
std::uint32_t get16(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8;
}
std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at) {
    return get16(b, at) | get16(b, at + 2) << 16;
}

// Raw deflate (no zlib header), as zip wants it; empty when it fails.
std::vector<std::uint8_t> deflateRaw(const std::vector<std::uint8_t>& in) {
    mz_stream s{};
    if (mz_deflateInit2(&s, MZ_DEFAULT_LEVEL, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9,
                        MZ_DEFAULT_STRATEGY) != MZ_OK)
        return {};
    std::vector<std::uint8_t> out(mz_deflateBound(&s, static_cast<mz_ulong>(in.size())) + 16);
    s.next_in = in.data();
    s.avail_in = static_cast<unsigned>(in.size());
    s.next_out = out.data();
    s.avail_out = static_cast<unsigned>(out.size());
    const int r = mz_deflate(&s, MZ_FINISH);
    const std::size_t n = s.total_out;
    mz_deflateEnd(&s);
    if (r != MZ_STREAM_END)
        return {};
    out.resize(n);
    return out;
}

bool inflateRaw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
    mz_stream s{};
    if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return false;
    s.next_in = in.data();
    s.avail_in = static_cast<unsigned>(in.size());
    s.next_out = out.data();
    s.avail_out = static_cast<unsigned>(out.size());
    const int r = mz_inflate(&s, MZ_FINISH);
    const bool ok = r == MZ_STREAM_END && s.total_out == out.size();
    mz_inflateEnd(&s);
    return ok;
}

std::uint32_t crcOf(std::span<const std::uint8_t> data) {
    return static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
}

}  // namespace

Result<std::vector<std::uint8_t>> zipBytes(const std::vector<ZipEntry>& entries) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    if (entries.size() > 0xFFFF)
        return Error{ErrorCode::Unsupported, "too many files for a zip archive"};
    std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const std::uint32_t dosTime =
        std::uint32_t(t.tm_hour) << 11 | std::uint32_t(t.tm_min) << 5 | std::uint32_t(t.tm_sec / 2);
    const std::uint32_t dosDate = std::uint32_t(std::max(0, t.tm_year - 80)) << 9 |
                                  std::uint32_t(t.tm_mon + 1) << 5 | std::uint32_t(t.tm_mday);
    std::vector<std::uint8_t> out, central;
    for (const ZipEntry& e : entries) {
        if (e.data.size() >= kMax || out.size() >= kMax || e.name.size() > 0xFFFF)
            return Error{ErrorCode::Unsupported, "\"" + e.name + "\" is too big for a zip archive"};
        const std::uint32_t crc = crcOf(e.data);
        std::vector<std::uint8_t> packed =
            e.data.empty() ? std::vector<std::uint8_t>{} : deflateRaw(e.data);
        const bool deflated = !packed.empty() && packed.size() < e.data.size();
        const std::vector<std::uint8_t>& body = deflated ? packed : e.data;
        const std::uint32_t method = deflated ? 8 : 0;
        const auto offset = static_cast<std::uint32_t>(out.size());
        auto header = [&](std::vector<std::uint8_t>& h, bool isCentral) {
            put32(h, isCentral ? kCentralSig : kLocalSig);
            if (isCentral)
                put16(h, 20);  // made by: version 2.0
            put16(h, 20);      // needed to extract
            put16(h, kUtf8Flag);
            put16(h, method);
            put16(h, dosTime);
            put16(h, dosDate);
            put32(h, crc);
            put32(h, static_cast<std::uint32_t>(body.size()));
            put32(h, static_cast<std::uint32_t>(e.data.size()));
            put16(h, static_cast<std::uint32_t>(e.name.size()));
            put16(h, 0);  // extra field
            if (isCentral) {
                put16(h, 0);  // comment
                put16(h, 0);  // disk
                put16(h, 0);  // internal attributes
                put32(h, 0);  // external attributes
                put32(h, offset);
            }
            h.insert(h.end(), e.name.begin(), e.name.end());
        };
        header(out, false);
        out.insert(out.end(), body.begin(), body.end());
        header(central, true);
    }
    if (out.size() + central.size() >= kMax)
        return Error{ErrorCode::Unsupported, "the files are too big for a zip archive"};
    const auto centralAt = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, kEndSig);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, centralAt);
    put16(out, 0);
    return out;
}

Result<void> writeZip(const std::filesystem::path& file, const std::vector<ZipEntry>& entries) {
    auto bytes = zipBytes(entries);
    if (!bytes)
        return bytes.error();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out)
        out.write(reinterpret_cast<const char*>(bytes.value().data()),
                  static_cast<std::streamsize>(bytes.value().size()));
    if (!out)
        return Error{ErrorCode::Io, "cannot write the zip file"};
    return {};
}

Result<std::vector<ZipEntry>> readZip(std::span<const std::uint8_t> b) {
    auto bad = [](const std::string& why) { return Error{ErrorCode::Parse, "bad zip: " + why}; };
    if (b.size() < 22)
        return bad("too short");
    std::size_t end = b.size() - 22;
    for (;; --end) {
        if (get32(b, end) == kEndSig)
            break;
        if (end == 0 || b.size() - end > 22 + 0xFFFF)
            return bad("no end record");
    }
    const std::uint32_t count = get16(b, end + 10);
    std::size_t at = get32(b, end + 16);
    std::vector<ZipEntry> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > b.size() || get32(b, at) != kCentralSig)
            return bad("central directory");
        const std::uint32_t method = get16(b, at + 10);
        const std::uint32_t crc = get32(b, at + 16);
        const std::uint32_t csize = get32(b, at + 20), usize = get32(b, at + 24);
        const std::uint32_t nameLen = get16(b, at + 28), extra = get16(b, at + 30),
                            comment = get16(b, at + 32);
        const std::uint32_t local = get32(b, at + 42);
        if (at + 46 + nameLen > b.size())
            return bad("name");
        ZipEntry e;
        e.name.assign(reinterpret_cast<const char*>(&b[at + 46]), nameLen);
        if (std::size_t(local) + 30 > b.size() || get32(b, local) != kLocalSig)
            return bad("local header of " + e.name);
        const std::size_t data = local + 30 + get16(b, local + 26) + get16(b, local + 28);
        if (data + csize > b.size())
            return bad("data of " + e.name);
        const auto body = b.subspan(data, csize);
        if (method == 0) {
            e.data.assign(body.begin(), body.end());
        } else if (method == 8) {
            e.data.resize(usize);
            if (!inflateRaw(body, e.data))
                return bad("cannot inflate " + e.name);
        } else {
            return bad("unsupported method in " + e.name);
        }
        if (e.data.size() != usize || crcOf(e.data) != crc)
            return bad("CRC of " + e.name);
        out.push_back(std::move(e));
        at += 46 + nameLen + extra + comment;
    }
    return out;
}

}  // namespace tbcad::io
