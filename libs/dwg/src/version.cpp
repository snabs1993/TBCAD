#include <cstring>

#include "tbcad/dwg/dwg.h"

namespace tbcad::dwg {

namespace {
struct Row {
    const char* code;
    Version version;
    const char* name;
    bool supported;
};
constexpr Row kRows[] = {
    {"AC1015", Version::R2000, "R2000", true}, {"AC1018", Version::R2004, "R2004", true},
    {"AC1021", Version::R2007, "R2007", true}, {"AC1024", Version::R2010, "R2010", true},
    {"AC1027", Version::R2013, "R2013", true}, {"AC1032", Version::R2018, "R2018", true},
    {"AC1012", Version::R13, "R13", false},    {"AC1014", Version::R14, "R14", false},
};
}  // namespace

VersionInfo detectVersion(std::span<const std::byte> bytes) {
    VersionInfo info;
    if (bytes.size() < 6)
        return info;
    char sig[7] = {};
    std::memcpy(sig, bytes.data(), 6);
    if (sig[0] != 'A' || sig[1] != 'C' || sig[2] != '1' || sig[3] != '0')
        return info;
    info.code = sig;
    for (const Row& r : kRows) {
        if (info.code == r.code) {
            info.version = r.version;
            info.name = r.name;
            info.supported = r.supported;
            return info;
        }
    }
    // AC1001..AC1009 are R2.x..R12 files; anything newer than AC1032 is unknown to us.
    if (sig[4] == '0' && sig[5] >= '0' && sig[5] <= '9') {
        info.version = Version::R12OrOlder;
        info.name = "R12 or older";
    } else {
        info.version = Version::Unknown;
        info.name = "unknown";
    }
    return info;
}

bool looksLikeDwg(std::span<const std::byte> bytes) {
    return !detectVersion(bytes).code.empty();
}

}  // namespace tbcad::dwg
