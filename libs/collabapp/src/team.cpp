#include "team.h"

#include <algorithm>
#include <cstring>

namespace tbcad::collabapp {

namespace {
void putU32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint32_t getU32(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}
}  // namespace

Bytes chatFrame(collab::PeerId from, const std::string& text) {
    Bytes b{kChat};
    putU32(b, static_cast<std::uint32_t>(from));
    const std::string t = text.substr(0, kMaxChatBytes);
    b.insert(b.end(), t.begin(), t.end());
    return b;
}

std::optional<ChatLine> readChat(const Bytes& f) {
    if (f.size() < 6 || f[0] != kChat || f.size() > 5 + kMaxChatBytes)
        return std::nullopt;
    ChatLine c;
    c.from = getU32(f, 1);
    c.text.assign(f.begin() + 5, f.end());
    return c;
}

Bytes restampChat(const Bytes& f, collab::PeerId from) {
    Bytes b = f;
    for (int i = 0; i < 4; ++i)
        b[1 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(from >> (8 * i));
    return b;
}

Bytes portFrame(std::uint8_t type, int port, const std::string& address) {
    Bytes b{type, static_cast<std::uint8_t>(port & 0xFF), static_cast<std::uint8_t>(port >> 8)};
    b.insert(b.end(), address.begin(), address.begin() + std::min<std::size_t>(address.size(), 64));
    return b;
}

std::optional<PortMessage> readPort(const Bytes& f, std::uint8_t type) {
    if (f.size() < 3 || f.size() > 3 + 64 || f[0] != type)
        return std::nullopt;
    PortMessage m;
    m.port = f[1] | (f[2] << 8);
    m.address.assign(f.begin() + 3, f.end());
    if (m.address.find_first_not_of("0123456789.") != std::string::npos)
        return std::nullopt;
    return m;
}

Bytes claimFrame(bool claim, LayerId layer) {
    Bytes b{kClaim, static_cast<std::uint8_t>(claim ? 1 : 0)};
    putU32(b, layer.value);
    return b;
}

std::optional<ClaimRequest> readClaim(const Bytes& f) {
    if (f.size() != 6 || f[0] != kClaim || f[1] > 1)
        return std::nullopt;
    return ClaimRequest{f[1] == 1, LayerId{getU32(f, 2)}};
}

bool Claims::claim(collab::PeerId who, LayerId layer) {
    auto it = owners_.find(layer.value);
    if (it != owners_.end() || owners_.size() >= kMaxClaims)
        return false;
    owners_[layer.value] = who;
    return true;
}

bool Claims::release(collab::PeerId who, LayerId layer) {
    auto it = owners_.find(layer.value);
    if (it == owners_.end() || it->second != who)
        return false;
    owners_.erase(it);
    return true;
}

bool Claims::releaseAll(collab::PeerId who) {
    return std::erase_if(owners_, [&](const auto& kv) { return kv.second == who; }) > 0;
}

std::optional<collab::PeerId> Claims::ownerOf(LayerId layer) const {
    auto it = owners_.find(layer.value);
    if (it == owners_.end())
        return std::nullopt;
    return it->second;
}

Bytes Claims::tableFrame() const {
    Bytes b{kClaimTable};
    putU32(b, static_cast<std::uint32_t>(owners_.size()));
    for (const auto& [layer, peer] : owners_) {
        putU32(b, layer);
        putU32(b, static_cast<std::uint32_t>(peer));
    }
    return b;
}

bool Claims::readTable(const Bytes& f) {
    if (f.size() < 5 || f[0] != kClaimTable)
        return false;
    const std::uint32_t n = getU32(f, 1);
    if (n > kMaxClaims || f.size() != 5 + std::size_t{n} * 8)
        return false;
    owners_.clear();
    for (std::uint32_t i = 0; i < n; ++i)
        owners_[getU32(f, 5 + std::size_t{i} * 8)] = getU32(f, 9 + std::size_t{i} * 8);
    return true;
}

}  // namespace tbcad::collabapp
