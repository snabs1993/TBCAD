#include "tbcad/ui/command_keys.h"

#include <cstdint>

namespace tbcad::ui {

namespace {

// The keys of a US keyboard, row by row, without and with Shift.
constexpr std::u32string_view kUsPlain = U"`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
constexpr std::u32string_view kUsShift = U"~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";

struct Layout {
    const char* id;
    const char* label;
    std::u32string_view plain;  // the same keys in this layout, in the order of kUsPlain
    std::u32string_view shift;
};

constexpr Layout kLayouts[] = {
    {"thai-kedmanee", "Thai (Kedmanee)",
     U"_ๅ/-ภถุึคตจขชๆไำพะัีรนยบลฃฟหกดเ้่าสวงผปแอิืทมใฝ",
     U"%+๑๒๓๔ู฿๕๖๗๘๙๐\"ฎฑธํ๊ณฯญฐ,ฅฤฆฏโฌ็๋ษศซ.()ฉฮฺ์?ฒฬฦ"},
};

static_assert(kUsPlain.size() == kUsShift.size());

constexpr bool layoutsFit() {
    for (const auto& l : kLayouts)
        if (l.plain.size() != kUsPlain.size() || l.shift.size() != kUsShift.size())
            return false;
    return true;
}
static_assert(layoutsFit(), "a layout must name every key of the US keyboard");

constexpr CommandKeyboard kChoices[] = {
    {"off", "Off (type commands in English)"},
    {"thai-kedmanee", "Thai (Kedmanee)"},
};

const Layout* find(std::string_view id) {
    for (const auto& l : kLayouts)
        if (id == l.id)
            return &l;
    return nullptr;
}

// Decodes one UTF-8 character at s[i] and advances i; a broken byte comes back as itself.
char32_t decode(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    int n = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + static_cast<std::size_t>(n) >= s.size()) {
        ++i;
        return b;
    }
    char32_t c = n == 0 ? b : n == 1 ? (b & 0x1F) : n == 2 ? (b & 0x0F) : (b & 0x07);
    for (int k = 1; k <= n; ++k)
        c = (c << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
    i += static_cast<std::size_t>(n) + 1;
    return c;
}

void encode(char32_t c, std::string& out) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::span<const CommandKeyboard> commandKeyboards() { return kChoices; }

char32_t commandKey(std::string_view id, char32_t ch, bool layoutTyping) {
    const Layout* l = find(id);
    if (!l || ch == U' ' || (ch < 0x80 && !layoutTyping))
        return ch;
    if (auto p = l->plain.find(ch); p != std::u32string_view::npos)
        return kUsPlain[p];
    if (auto p = l->shift.find(ch); p != std::u32string_view::npos)
        return kUsShift[p];
    return ch;
}

std::string toCommandKeys(std::string_view id, std::string_view utf8, bool layoutTyping) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();)
        encode(commandKey(id, decode(utf8, i), layoutTyping), out);
    return out;
}

}  // namespace tbcad::ui
