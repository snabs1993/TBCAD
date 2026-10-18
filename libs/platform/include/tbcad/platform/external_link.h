#pragma once

#include <string>
#include <string_view>

namespace tbcad::platform {

// What a hyperlink target points at. Only web addresses, mail addresses, documents and folders
// may be handed to the operating system; programs, scripts, network shares and unknown schemes
// are refused so a drawing cannot make the program run something.
enum class LinkKind { Web, Mail, File, Folder, Refused };

struct LinkCheck {
    LinkKind kind = LinkKind::Refused;
    std::string target;  // what to hand to the system (a file: address becomes a plain path)
    std::string reason;  // why it was refused
};

LinkCheck checkLink(std::string_view text);

// Hands an accepted target to the system's default handler. False when that fails or when this
// platform has no handler. Callers ask the user first.
bool openExternal(const LinkCheck& link);

}  // namespace tbcad::platform
