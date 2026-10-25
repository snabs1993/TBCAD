#include <algorithm>

#include "text_internal.h"

namespace tbcad::text {

namespace detail {

// Decodes one code point at s[i]; returns its byte length (1 for an invalid byte).
std::size_t decodeAt(std::string_view s, std::size_t i, char32_t& cp) {
    auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    char32_t c = 0xFFFD;
    if (b < 0x80) {
        c = b;
    } else if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
        c = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        c = b & 0x0F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        c = b & 0x07;
    } else {
        cp = 0xFFFD;
        return 1;
    }
    if (len > 1) {
        if (i + len > s.size()) {
            cp = 0xFFFD;
            return 1;
        }
        for (std::size_t k = 1; k < len; ++k) {
            auto cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) {
                cp = 0xFFFD;
                return 1;
            }
            c = (c << 6) | (cb & 0x3F);
        }
    }
    cp = c;
    return len;
}

bool isExtend(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) ||                                  // combining diacritics
           (c >= 0x0483 && c <= 0x0489) ||                                  // Cyrillic marks
           (c >= 0x0591 && c <= 0x05BD) || (c >= 0x05BF && c <= 0x05C7) ||  // Hebrew points
           (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||                   // Arabic marks
           c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) ||  // Thai vowels above / below
           c == 0x0E33 ||                                  // Thai SARA AM
           (c >= 0x0E47 && c <= 0x0E4E) ||                 // Thai tone marks, MAITAIKHU ...
           c == 0x0EB1 || (c >= 0x0EB4 && c <= 0x0EBC) || (c >= 0x0EC8 && c <= 0x0ECD) ||  // Lao
           (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200C || c == 0x200D;
}

bool isPrepend(char32_t c) {
    return c >= 0x0E40 && c <= 0x0E44;  // Thai SARA E .. SARA AI MAIMALAI
}

}  // namespace detail

std::vector<std::size_t> clusterBoundaries(std::string_view s) {
    std::vector<std::size_t> out{0};
    std::size_t i = 0;
    char32_t prev = 0;
    bool first = true;
    while (i < s.size()) {
        char32_t cp = 0;
        std::size_t len = detail::decodeAt(s, i, cp);
        if (!first && !detail::isExtend(cp) && !detail::isPrepend(prev))
            out.push_back(i);
        // A ZWJ keeps the character after it in the cluster (emoji / conjuncts).
        prev = (cp == 0x200D) ? char32_t{0x0E40} : cp;
        first = false;
        i += len;
    }
    if (s.size() > 0)
        out.push_back(s.size());
    return out;
}

namespace {

// Byte offset of the code point that ends at `end` (end > 0), stepping over UTF-8 continuation
// bytes; an invalid sequence counts one byte, as decodeAt does.
std::size_t codePointBefore(std::string_view s, std::size_t end) {
    std::size_t i = end - 1;
    for (std::size_t back = 0;
         i > 0 && back < 3 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; ++back)
        --i;
    char32_t cp = 0;
    if (i + detail::decodeAt(s, i, cp) != end)
        return end - 1;
    return i;
}

}  // namespace

std::size_t nextClusterStart(std::string_view s, std::size_t idx) {
    if (idx >= s.size())
        return s.size();
    char32_t cp = 0;
    std::size_t i = idx + detail::decodeAt(s, idx, cp);
    char32_t prev = (cp == 0x200D) ? char32_t{0x0E40} : cp;
    while (i < s.size()) {
        const std::size_t len = detail::decodeAt(s, i, cp);
        if (!detail::isExtend(cp) && !detail::isPrepend(prev))
            break;
        prev = (cp == 0x200D) ? char32_t{0x0E40} : cp;
        i += len;
    }
    return i;
}

std::size_t prevClusterStart(std::string_view s, std::size_t idx) {
    if (idx == 0)
        return 0;
    idx = std::min(idx, s.size());
    std::size_t p = codePointBefore(s, idx);
    while (p > 0) {
        char32_t cp = 0;
        detail::decodeAt(s, p, cp);
        const std::size_t before = codePointBefore(s, p);
        char32_t pc = 0;
        detail::decodeAt(s, before, pc);
        if (pc == 0x200D)
            pc = 0x0E40;
        if (!detail::isExtend(cp) && !detail::isPrepend(pc))
            break;
        p = before;
    }
    return p;
}

}  // namespace tbcad::text
