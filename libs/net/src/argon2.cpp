#include "tbcad/net/argon2.h"

#include <array>
#include <cstring>

namespace tbcad::net {

namespace {

// ---- BLAKE2b ----------------------------------------------------------------------------------

constexpr std::uint64_t kIv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

constexpr std::uint8_t kSigma[12][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                         {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                                         {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
                                         {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                                         {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
                                         {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                                         {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
                                         {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                                         {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
                                         {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
                                         {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                         {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

inline std::uint64_t rotr(std::uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

inline std::uint64_t load64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

inline void store64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v & 0xff);
        v >>= 8;
    }
}

inline void store32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v & 0xff);
        v >>= 8;
    }
}

struct Blake2b {
    std::uint64_t h[8];
    std::uint64_t t = 0;  // bytes hashed so far (messages here are far below 2^64)
    std::uint8_t buf[128];
    std::size_t len = 0;
    std::size_t outLen;

    explicit Blake2b(std::size_t outBytes) : outLen(outBytes) {
        std::memcpy(h, kIv, sizeof h);
        h[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(outBytes);
    }

    void compress(const std::uint8_t* block, bool last) {
        std::uint64_t m[16], v[16];
        for (int i = 0; i < 16; ++i)
            m[i] = load64(block + 8 * i);
        for (int i = 0; i < 8; ++i) {
            v[i] = h[i];
            v[i + 8] = kIv[i];
        }
        v[12] ^= t;
        if (last)
            v[14] = ~v[14];
        auto g = [&](int a, int b, int c, int d, std::uint64_t x, std::uint64_t y) {
            v[a] = v[a] + v[b] + x;
            v[d] = rotr(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];
            v[b] = rotr(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + y;
            v[d] = rotr(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = rotr(v[b] ^ v[c], 63);
        };
        for (int r = 0; r < 12; ++r) {
            const std::uint8_t* s = kSigma[r];
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i)
            h[i] ^= v[i] ^ v[i + 8];
    }

    void update(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        while (size > 0) {
            if (len == 128) {  // keep the last block back: it needs the final flag
                t += 128;
                compress(buf, false);
                len = 0;
            }
            std::size_t n = 128 - len < size ? 128 - len : size;
            std::memcpy(buf + len, p, n);
            len += n;
            p += n;
            size -= n;
        }
    }

    std::vector<std::uint8_t> finish() {
        t += len;
        std::memset(buf + len, 0, 128 - len);
        compress(buf, true);
        std::uint8_t full[64];
        for (int i = 0; i < 8; ++i)
            store64(full + 8 * i, h[i]);
        return std::vector<std::uint8_t>(full, full + outLen);
    }
};

// ---- Argon2 -----------------------------------------------------------------------------------

constexpr std::size_t kBlockWords = 128;  // 1024 bytes
using Block = std::array<std::uint64_t, kBlockWords>;

// H' of RFC 9106 section 3.3: BLAKE2b extended to any output length.
std::vector<std::uint8_t> hPrime(const std::uint8_t* in, std::size_t inLen, std::uint32_t outLen) {
    std::uint8_t le[4];
    store32(le, outLen);
    if (outLen <= 64) {
        Blake2b b(outLen);
        b.update(le, 4);
        b.update(in, inLen);
        return b.finish();
    }
    std::vector<std::uint8_t> out;
    out.reserve(outLen);
    Blake2b first(64);
    first.update(le, 4);
    first.update(in, inLen);
    std::vector<std::uint8_t> v = first.finish();
    out.insert(out.end(), v.begin(), v.begin() + 32);
    std::uint32_t remaining = outLen - 32;
    while (remaining > 64) {
        v = blake2b(v.data(), v.size(), 64);
        out.insert(out.end(), v.begin(), v.begin() + 32);
        remaining -= 32;
    }
    v = blake2b(v.data(), v.size(), remaining);
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

inline std::uint64_t fBlaMka(std::uint64_t x, std::uint64_t y) {
    const std::uint64_t m = 0xFFFFFFFFULL;
    return x + y + 2 * (x & m) * (y & m);
}

inline void gb(std::uint64_t& a, std::uint64_t& b, std::uint64_t& c, std::uint64_t& d) {
    a = fBlaMka(a, b);
    d = rotr(d ^ a, 32);
    c = fBlaMka(c, d);
    b = rotr(b ^ c, 24);
    a = fBlaMka(a, b);
    d = rotr(d ^ a, 16);
    c = fBlaMka(c, d);
    b = rotr(b ^ c, 63);
}

// The permutation P on sixteen 64-bit words.
void permute(std::uint64_t* v0, std::uint64_t* v1, std::uint64_t* v2, std::uint64_t* v3,
             std::uint64_t* v4, std::uint64_t* v5, std::uint64_t* v6, std::uint64_t* v7,
             std::uint64_t* v8, std::uint64_t* v9, std::uint64_t* v10, std::uint64_t* v11,
             std::uint64_t* v12, std::uint64_t* v13, std::uint64_t* v14, std::uint64_t* v15) {
    gb(*v0, *v4, *v8, *v12);
    gb(*v1, *v5, *v9, *v13);
    gb(*v2, *v6, *v10, *v14);
    gb(*v3, *v7, *v11, *v15);
    gb(*v0, *v5, *v10, *v15);
    gb(*v1, *v6, *v11, *v12);
    gb(*v2, *v7, *v8, *v13);
    gb(*v3, *v4, *v9, *v14);
}

// next = G(prev, ref) (xor the old content of `next` when `withXor`).
void fillBlock(const Block& prev, const Block& ref, Block& next, bool withXor) {
    Block r, z;
    for (std::size_t i = 0; i < kBlockWords; ++i)
        r[i] = ref[i] ^ prev[i];
    z = r;
    // rows: 16 consecutive words
    for (int i = 0; i < 8; ++i) {
        std::uint64_t* w = z.data() + 16 * i;
        permute(w + 0, w + 1, w + 2, w + 3, w + 4, w + 5, w + 6, w + 7, w + 8, w + 9, w + 10,
                w + 11, w + 12, w + 13, w + 14, w + 15);
    }
    // columns: words 2i, 2i+1 of each of the eight rows
    for (int i = 0; i < 8; ++i) {
        std::uint64_t* w = z.data() + 2 * i;
        permute(w + 0, w + 1, w + 16, w + 17, w + 32, w + 33, w + 48, w + 49, w + 64, w + 65,
                w + 80, w + 81, w + 96, w + 97, w + 112, w + 113);
    }
    if (withXor) {
        for (std::size_t i = 0; i < kBlockWords; ++i)
            next[i] ^= z[i] ^ r[i];
    } else {
        for (std::size_t i = 0; i < kBlockWords; ++i)
            next[i] = z[i] ^ r[i];
    }
}

struct Instance {
    std::vector<Block> memory;
    std::uint32_t passes, lanes, laneLength, segmentLength, memoryBlocks;
};

std::uint32_t indexAlpha(const Instance& in, std::uint32_t pass, std::uint32_t slice,
                         std::uint32_t index, std::uint32_t j1, bool sameLane) {
    std::uint32_t area;
    if (pass == 0) {
        if (slice == 0)
            area = index - 1;
        else if (sameLane)
            area = slice * in.segmentLength + index - 1;
        else
            area = slice * in.segmentLength + (index == 0 ? static_cast<std::uint32_t>(-1) : 0);
    } else {
        if (sameLane)
            area = in.laneLength - in.segmentLength + index - 1;
        else
            area = in.laneLength - in.segmentLength +
                   (index == 0 ? static_cast<std::uint32_t>(-1) : 0);
    }
    std::uint64_t rel = j1;
    rel = (rel * rel) >> 32;
    rel = area - 1 - ((static_cast<std::uint64_t>(area) * rel) >> 32);
    std::uint32_t start = 0;
    if (pass != 0)
        start = slice == 3 ? 0 : (slice + 1) * in.segmentLength;
    return static_cast<std::uint32_t>((start + rel) % in.laneLength);
}

void fillSegment(Instance& in, std::uint32_t pass, std::uint32_t lane, std::uint32_t slice) {
    const bool independent = pass == 0 && slice < 2;  // Argon2id: first half of pass 0
    Block zero{}, input{}, address{};
    std::uint32_t start = 0;
    if (independent) {
        input[0] = pass;
        input[1] = lane;
        input[2] = slice;
        input[3] = in.memoryBlocks;
        input[4] = in.passes;
        input[5] = 2;  // Argon2id
    }
    auto nextAddresses = [&] {
        ++input[6];
        fillBlock(zero, input, address, false);
        fillBlock(zero, address, address, false);
    };
    if (pass == 0 && slice == 0) {
        start = 2;
        if (independent)
            nextAddresses();
    }
    std::uint32_t cur = lane * in.laneLength + slice * in.segmentLength + start;
    std::uint32_t prev = (cur % in.laneLength == 0) ? cur + in.laneLength - 1 : cur - 1;
    for (std::uint32_t i = start; i < in.segmentLength; ++i, ++cur, ++prev) {
        if (cur % in.laneLength == 1)
            prev = cur - 1;
        std::uint64_t rnd;
        if (independent) {
            if (i % kBlockWords == 0)
                nextAddresses();
            rnd = address[i % kBlockWords];
        } else {
            rnd = in.memory[prev][0];
        }
        std::uint32_t refLane = static_cast<std::uint32_t>((rnd >> 32) % in.lanes);
        if (pass == 0 && slice == 0)
            refLane = lane;
        std::uint32_t refIndex = indexAlpha(
            in, pass, slice, i, static_cast<std::uint32_t>(rnd & 0xFFFFFFFFULL), refLane == lane);
        const Block& ref = in.memory[refLane * in.laneLength + refIndex];
        if (pass == 0)
            fillBlock(in.memory[prev], ref, in.memory[cur], false);
        else
            fillBlock(in.memory[prev], ref, in.memory[cur], true);
    }
}

void putLe32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    std::uint8_t b[4];
    store32(b, x);
    v.insert(v.end(), b, b + 4);
}

}  // namespace

std::vector<std::uint8_t> blake2b(const void* data, std::size_t size, std::size_t outLen) {
    if (outLen == 0 || outLen > 64)
        return {};
    Blake2b b(outLen);
    b.update(data, size);
    return b.finish();
}

std::vector<std::uint8_t> argon2id(const std::string& password,
                                   const std::vector<std::uint8_t>& salt, const Argon2Params& p) {
    if (p.lanes == 0 || p.lanes > 0xFFFFFF || p.timeCost == 0 || p.tagLength < 4 ||
        salt.size() < 8 || p.memoryKiB < 8 * p.lanes)
        return {};
    Instance in;
    in.passes = p.timeCost;
    in.lanes = p.lanes;
    in.memoryBlocks = (p.memoryKiB / (4 * p.lanes)) * (4 * p.lanes);
    in.laneLength = in.memoryBlocks / p.lanes;
    in.segmentLength = in.laneLength / 4;
    in.memory.assign(in.memoryBlocks, Block{});

    std::vector<std::uint8_t> h0in;
    putLe32(h0in, p.lanes);
    putLe32(h0in, p.tagLength);
    putLe32(h0in, p.memoryKiB);
    putLe32(h0in, p.timeCost);
    putLe32(h0in, 0x13);
    putLe32(h0in, 2);  // Argon2id
    putLe32(h0in, static_cast<std::uint32_t>(password.size()));
    h0in.insert(h0in.end(), password.begin(), password.end());
    putLe32(h0in, static_cast<std::uint32_t>(salt.size()));
    h0in.insert(h0in.end(), salt.begin(), salt.end());
    putLe32(h0in, 0);  // no secret
    putLe32(h0in, 0);  // no associated data
    std::vector<std::uint8_t> h0 = blake2b(h0in.data(), h0in.size(), 64);
    h0.resize(72);

    for (std::uint32_t lane = 0; lane < p.lanes; ++lane) {
        for (std::uint32_t col = 0; col < 2; ++col) {
            store32(h0.data() + 64, col);
            store32(h0.data() + 68, lane);
            std::vector<std::uint8_t> bytes = hPrime(h0.data(), h0.size(), 1024);
            Block& b = in.memory[lane * in.laneLength + col];
            for (std::size_t i = 0; i < kBlockWords; ++i)
                b[i] = load64(bytes.data() + 8 * i);
        }
    }

    for (std::uint32_t pass = 0; pass < p.timeCost; ++pass)
        for (std::uint32_t slice = 0; slice < 4; ++slice)
            for (std::uint32_t lane = 0; lane < p.lanes; ++lane)
                fillSegment(in, pass, lane, slice);

    Block fin = in.memory[in.laneLength - 1];
    for (std::uint32_t lane = 1; lane < p.lanes; ++lane) {
        const Block& last = in.memory[lane * in.laneLength + in.laneLength - 1];
        for (std::size_t i = 0; i < kBlockWords; ++i)
            fin[i] ^= last[i];
    }
    std::vector<std::uint8_t> bytes(1024);
    for (std::size_t i = 0; i < kBlockWords; ++i)
        store64(bytes.data() + 8 * i, fin[i]);
    return hPrime(bytes.data(), bytes.size(), p.tagLength);
}

}  // namespace tbcad::net
