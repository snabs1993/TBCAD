#pragma once
// UTF-8 checks for text that comes from outside (typed input, scripts, other programs).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tbcad::utf8 {

// Length of the well-formed sequence starting at s[i], or 0 when it is not one (overlong forms,
// surrogates and code points past U+10FFFF included).
inline std::size_t sequenceAt(std::string_view s, std::size_t i) {
    const auto c = static_cast<std::uint8_t>(s[i]);
    std::size_t len;
    std::uint32_t cp;
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > s.size())
        return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto cc = static_cast<std::uint8_t>(s[i + k]);
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

inline bool isValid(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = sequenceAt(s, i);
        if (len == 0)
            return false;
        i += len;
    }
    return true;
}

// The text with every byte that is not part of a well-formed sequence replaced by U+FFFD.
inline std::string repaired(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = sequenceAt(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

}  // namespace tbcad::utf8
