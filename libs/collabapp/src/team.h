#pragma once

// Team controls carried next to the session engine (they are not part of the edit log):
// layer claims ("this layer is mine, others may not change it") and chat. The host keeps the
// authoritative claim table and checks every guest edit against it (Host::policy).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/collab/ops.h"
#include "tbcad/core/document.h"

namespace tbcad::collabapp {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t kChat = 0xF1;         // [type][u32 peer][text]
inline constexpr std::uint8_t kClaim = 0xF2;        // [type][u8 claim][u32 layer]  guest -> host
inline constexpr std::uint8_t kClaimTable = 0xF3;   // [type][u32 n][(u32 layer, u32 peer) * n]
// Host hand-over: the host asks a guest to take over (kBecomeHost), the guest answers with the
// port it now listens on (kHostReady, port 0 = cannot), and the old host tells everybody else
// where to go (kMoveTo: [type][u16 port][address]) before joining the new host itself.
inline constexpr std::uint8_t kBecomeHost = 0xF4;
inline constexpr std::uint8_t kHostReady = 0xF5;
inline constexpr std::uint8_t kMoveTo = 0xF6;
inline constexpr std::size_t kMaxChatBytes = 500;
inline constexpr std::size_t kMaxClaims = 4096;

struct ChatLine {
    collab::PeerId from = 0;
    std::string text{};
};

Bytes chatFrame(collab::PeerId from, const std::string& text);
std::optional<ChatLine> readChat(const Bytes& f);
// `from` replaced by the host with the sender it knows (guests cannot speak for others).
Bytes restampChat(const Bytes& f, collab::PeerId from);

Bytes claimFrame(bool claim, LayerId layer);
struct ClaimRequest {
    bool claim = true;
    LayerId layer{};
};
std::optional<ClaimRequest> readClaim(const Bytes& f);

Bytes portFrame(std::uint8_t type, int port, const std::string& address = {});
struct PortMessage {
    int port = 0;
    std::string address{};
};
std::optional<PortMessage> readPort(const Bytes& f, std::uint8_t type);

class Claims {
public:
    // Host side. True when the table changed.
    bool claim(collab::PeerId who, LayerId layer);
    bool release(collab::PeerId who, LayerId layer);
    bool releaseAll(collab::PeerId who);

    std::optional<collab::PeerId> ownerOf(LayerId layer) const;
    const std::map<std::uint32_t, collab::PeerId>& table() const { return owners_; }

    Bytes tableFrame() const;
    // Guest side: replaces the table with the host's. False for a damaged frame.
    bool readTable(const Bytes& f);

    // Host::policy: refuses an edit by `from` that changes an object on (or moves one off) a layer
    // somebody else claimed. `nameOf` turns a peer into a name for the message.
    template <class NameOf>
    std::string check(collab::PeerId from, const collab::Op& op, const Document& doc,
                      NameOf nameOf) const {
        auto blocked = [&](LayerId layer) -> std::string {
            auto owner = ownerOf(layer);
            if (!owner || *owner == from)
                return {};
            const Layer* l = doc.layer(layer);
            return "layer \"" + (l ? l->name : std::string("?")) + "\" is claimed by " +
                   nameOf(*owner);
        };
        for (const io::EntityDelta& e : op.delta.entities) {
            if (const Entity* now = doc.find(e.id))
                if (std::string why = blocked(now->common.layer); !why.empty())
                    return why;
            if (e.after)
                if (std::string why = blocked(e.after->common.layer); !why.empty())
                    return why;
        }
        return {};
    }

private:
    std::map<std::uint32_t, collab::PeerId> owners_{};
};

}  // namespace tbcad::collabapp
