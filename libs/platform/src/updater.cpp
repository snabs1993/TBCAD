#include "tbcad/platform/updater.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>

#include "tbcad/platform/json.h"

namespace tbcad::platform {
namespace {

bool allDigits(std::string_view s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::vector<std::string_view> splitDots(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    for (;;) {
        const auto dot = s.find('.', at);
        out.push_back(s.substr(at, dot == std::string_view::npos ? dot : dot - at));
        if (dot == std::string_view::npos)
            return out;
        at = dot + 1;
    }
}

int comparePre(const std::string& a, const std::string& b) {
    if (a.empty() != b.empty())
        return a.empty() ? 1 : -1;  // a final release is newer than its test builds
    const auto pa = splitDots(a), pb = splitDots(b);
    for (std::size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        if (i >= pa.size())
            return -1;
        if (i >= pb.size())
            return 1;
        const bool na = allDigits(pa[i]), nb = allDigits(pb[i]);
        if (na && nb) {
            const long long x = std::stoll(std::string(pa[i].substr(0, 9)));
            const long long y = std::stoll(std::string(pb[i].substr(0, 9)));
            if (x != y)
                return x < y ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;  // numbers sort before words
        } else if (int c = pa[i].compare(pb[i]); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

bool isSetupName(const std::string& n) {
    constexpr std::string_view head = "TBCAD-", tail = "-setup.exe";
    return n.size() > head.size() + tail.size() && n.compare(0, head.size(), head) == 0 &&
           n.compare(n.size() - tail.size(), tail.size(), tail) == 0 &&
           n.find_first_of("/\\") == std::string::npos;
}

}  // namespace

Version parseVersion(std::string_view text) {
    Version v;
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
        text.remove_prefix(1);
    const auto dash = text.find('-');
    std::string_view num = text.substr(0, dash);
    if (dash != std::string_view::npos)
        v.pre = std::string(text.substr(dash + 1));
    const auto parts = splitDots(num);
    if (parts.size() < 1 || parts.size() > 4)
        return v;
    int n[3] = {0, 0, 0};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!allDigits(parts[i]) || parts[i].size() > 6)
            return v;
        if (i < 3)
            n[i] = std::stoi(std::string(parts[i]));
    }
    v.major = n[0];
    v.minor = n[1];
    v.patch = n[2];
    v.valid = true;
    return v;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    return comparePre(a.pre, b.pre);
}

std::vector<ReleaseInfo> parseReleases(std::string_view json, std::string_view allowedPrefix) {
    std::vector<ReleaseInfo> out;
    auto parsed = parseJson(json);
    if (!parsed || !parsed.value().isArray())
        return out;
    for (const Json& r : parsed.value().asArray()) {
        if (!r.isObject())
            continue;
        const auto str = [&](const Json& o, const char* k) {
            const Json* f = o.find(k);
            return f && f->isString() ? f->asString() : std::string();
        };
        const auto flag = [&](const char* k) {
            const Json* f = r.find(k);
            return f && f->isBool() && f->asBool();
        };
        if (flag("draft"))
            continue;
        ReleaseInfo info;
        info.tag = str(r, "tag_name");
        const Version v = parseVersion(info.tag);
        if (!v.valid)
            continue;
        info.version = info.tag.front() == 'v' ? info.tag.substr(1) : info.tag;
        info.pageUrl = str(r, "html_url");
        info.notes = str(r, "body");
        info.prerelease = flag("prerelease") || !v.pre.empty();
        const Json* assets = r.find("assets");
        if (!assets)
            continue;
        for (const Json& a : assets->asArray()) {
            const std::string name = str(a, "name"), url = str(a, "browser_download_url");
            if (!isSetupName(name) || url.compare(0, allowedPrefix.size(), allowedPrefix) != 0)
                continue;
            info.assetName = name;
            info.assetUrl = url;
            if (const Json* sz = a.find("size"))
                info.assetSize = static_cast<long long>(sz->asNumber());
            std::string digest = str(a, "digest");  // "sha256:<hex>"
            if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64)
                info.sha256 = digest.substr(7);
            std::transform(info.sha256.begin(), info.sha256.end(), info.sha256.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            out.push_back(info);
            break;
        }
    }
    return out;
}

std::optional<ReleaseInfo> pickUpdate(const std::vector<ReleaseInfo>& releases,
                                      std::string_view current, std::string_view channel) {
    const Version cur = parseVersion(current);
    if (!cur.valid)
        return std::nullopt;
    const bool test = channel == "test" || (channel != "stable" && !cur.pre.empty());
    const ReleaseInfo* best = nullptr;
    Version bestV;
    for (const auto& r : releases) {
        if (!test && r.prerelease)
            continue;
        const Version v = parseVersion(r.version);
        if (compareVersions(v, cur) <= 0)
            continue;
        if (!best || compareVersions(v, bestV) > 0) {
            best = &r;
            bestV = v;
        }
    }
    return best ? std::optional<ReleaseInfo>(*best) : std::nullopt;
}

namespace {
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buf{};
    std::size_t fill = 0;
    std::uint64_t total = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char* p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const auto t1 =
                hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 =
                (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void add(const unsigned char* p, std::size_t n) {
        total += n;
        while (n > 0) {
            const std::size_t take = std::min(n, buf.size() - fill);
            std::copy(p, p + take, buf.begin() + static_cast<std::ptrdiff_t>(fill));
            fill += take;
            p += take;
            n -= take;
            if (fill == 64) {
                block(buf.data());
                fill = 0;
            }
        }
    }

    std::string finish() {
        const std::uint64_t bits = total * 8;
        const unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (fill != 56)
            add(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        add(len, 8);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        for (std::uint32_t v : h)
            for (int s = 28; s >= 0; s -= 4)
                out.push_back(hex[(v >> s) & 0xf]);
        return out;
    }
};
}  // namespace

std::string sha256Hex(std::string_view bytes) {
    Sha256 s;
    s.add(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return s.finish();
}

std::string sha256FileHex(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    Sha256 s;
    std::array<char, 1 << 16> chunk;
    while (in.read(chunk.data(), chunk.size()) || in.gcount() > 0)
        s.add(reinterpret_cast<const unsigned char*>(chunk.data()),
              static_cast<std::size_t>(in.gcount()));
    if (in.bad())
        return {};
    return s.finish();
}

std::vector<std::string> installerArguments(bool perMachine) {
    return {"/SILENT",        "/SUPPRESSMSGBOXES",
            "/NORESTART",     "/CLOSEAPPLICATIONS",
            "/TBCADUPDATE=1", perMachine ? "/ALLUSERS" : "/CURRENTUSER"};
}

// ---- UpdateService ----------------------------------------------------------------------------

UpdateService::~UpdateService() {
    cancel_ = true;
    join();
}

void UpdateService::join() {
    if (worker_.joinable())
        worker_.join();
}

std::optional<ReleaseInfo> UpdateService::release() const {
    std::lock_guard lock(mu_);
    return release_;
}
std::filesystem::path UpdateService::installer() const {
    std::lock_guard lock(mu_);
    return installer_;
}
std::string UpdateService::error() const {
    std::lock_guard lock(mu_);
    return error_;
}

void UpdateService::cancel() {
    cancel_ = true;
}

void UpdateService::check(std::string currentVersion, std::string channel) {
    const State s = state_.load();
    if (s == State::Checking || s == State::Downloading)
        return;
    join();
    cancel_ = false;
    state_ = State::Checking;
    worker_ = std::thread(
        [this, currentVersion = std::move(currentVersion), channel = std::move(channel)] {
            std::string body, err;
            if (!httpsGet(std::string(kReleaseFeedUrl), body, err)) {
                std::lock_guard lock(mu_);
                error_ = err;
                state_ = State::Failed;
                return;
            }
            auto pick = pickUpdate(parseReleases(body), currentVersion, channel);
            std::lock_guard lock(mu_);
            release_ = pick;
            state_ = pick ? State::Available : State::UpToDate;
        });
}

void UpdateService::download(const std::filesystem::path& folder) {
    if (state_.load() != State::Available)
        return;
    const auto rel = release();
    if (!rel)
        return;
    join();
    cancel_ = false;
    progress_ = 0;
    state_ = State::Downloading;
    worker_ = std::thread([this, rel = *rel, folder] {
        const auto fail = [&](std::string why) {
            std::lock_guard lock(mu_);
            error_ = std::move(why);
            state_ = State::Failed;
        };
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const auto file = folder / rel.assetName;
        std::string err;
        if (!httpsDownload(rel.assetUrl, file, &progress_, &cancel_, err)) {
            std::filesystem::remove(file, ec);
            return fail(cancel_ ? "Download cancelled." : err);
        }
        if (rel.assetSize > 0 &&
            static_cast<long long>(std::filesystem::file_size(file, ec)) != rel.assetSize) {
            std::filesystem::remove(file, ec);
            return fail("The downloaded file has the wrong size.");
        }
        if (!rel.sha256.empty() && sha256FileHex(file) != rel.sha256) {
            std::filesystem::remove(file, ec);
            return fail("The downloaded file does not match its checksum; it was deleted.");
        }
        std::lock_guard lock(mu_);
        installer_ = file;
        state_ = State::Ready;
    });
}

}  // namespace tbcad::platform
