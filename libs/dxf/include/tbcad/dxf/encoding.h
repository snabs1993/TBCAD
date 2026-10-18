// Text encodings found in DXF files and conversion of DXF text to UTF-8.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "tbcad/dxf/report.h"

namespace tbcad::dxf {

class Lexer;

enum class Encoding {
    Utf8,
    Cp874,   // ANSI_874, Thai
    Cp932,   // Japanese (Shift-JIS)
    Cp936,   // Simplified Chinese (GBK)
    Cp949,   // Korean
    Cp950,   // Traditional Chinese (Big5)
    Cp1361,  // Korean (Johab)
    Cp1250,  // Central European
    Cp1251,  // Cyrillic
    Cp1252,  // Western European (the DXF default)
    Cp1253,  // Greek
    Cp1254,  // Turkish
    Cp1255,  // Hebrew
    Cp1256,  // Arabic
    Cp1257,  // Baltic
    Cp1258,  // Vietnamese
};

// "UTF-8", "ANSI_874", ... (the spelling used by $DWGCODEPAGE).
std::string_view encodingName(Encoding e);

// Parses a $DWGCODEPAGE value ("ANSI_874", "ansi_1252", "UTF-8", "8859_1" ...).
std::optional<Encoding> encodingFromName(std::string_view dwgCodepage);

// Values of the $ACADVER and $DWGCODEPAGE header variables, empty when absent.
struct HeaderHints {
    std::string acadVer;
    std::string dwgCodepage;
};

// Reads $ACADVER and $DWGCODEPAGE by scanning the HEADER section only (cheap).
HeaderHints scanHeaderHints(std::span<const std::byte> bytes);
// The same, from a lexer positioned at the start (it is consumed).
HeaderHints scanHeaderHints(Lexer& lexer);

// Chooses the text encoding of a DXF file. Order (research/dxf-real-world.md section 2):
//   1. UTF-8 byte order mark.
//   2. AC1021 (R2007) and later: valid UTF-8 -> UTF-8; $DWGCODEPAGE is never trusted there.
//   3. $DWGCODEPAGE, when it names a known codepage. Two refinements, because exporters
//      often leave the 1252 default in place: if the file is valid UTF-8 with non-ASCII text,
//      or its high bytes look like Thai, those win over a declared (or missing) ANSI_1252.
//   4. No usable $DWGCODEPAGE: UTF-8 if valid with non-ASCII text, ANSI_874 when bytes
//      0xA1-0xFB dominate in runs, else ANSI_1252.
// Every decision that is a guess is recorded in `report` (code dxf.encoding.guess).
Encoding detectEncoding(std::span<const std::byte> bytes, std::string_view acadVer,
                        std::string_view dwgCodepage, Report& report);

// Converts raw DXF text to UTF-8. Expands \U+nnnn (including surrogate pairs) and \M+nxxxx
// (n = 1 cp932, 2 cp950, 3 cp949, 5 cp936). Bytes that are not valid in `from` become
// U+FFFD, so the result is always valid UTF-8.
std::string decodeText(std::string_view raw, Encoding from);

// Replaces the "%%" codes of TEXT/ATTRIB strings in UTF-8 text: %%c -> U+2300, %%d -> U+00B0,
// %%p -> U+00B1, %%% -> %, %%nnn -> code point nnn; %%u %%o %%k toggles are removed.
std::string replacePercentCodes(std::string_view utf8, bool keepRules = false);

// Plain text of an MTEXT string (UTF-8 in, UTF-8 out): \P -> newline, formatting codes
// (\f \H \C \W \T \Q \A \p ... ending in ';', and \L \l \O \o \K \k) and {} groups removed,
// \S stacks flattened ("1/2"), "%%" codes replaced as in replacePercentCodes.
std::string stripMText(std::string_view utf8);

}  // namespace tbcad::dxf
