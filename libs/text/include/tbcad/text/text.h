#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/font/font_library.h"
#include "tbcad/geom/entity.h"

// Text for TBCAD: font lookup per text style, TEXT / MTEXT layout, grapheme-cluster-safe word
// wrapping (Thai included), and the display generators for EntityKind::Text and ::MText.
// Behaviour is documented in docs/text.md.
namespace tbcad::text {

// ---- fonts -------------------------------------------------------------------------------------

// The process-wide font library (thread-safe). Bundled Sarabun is the default font.
font::FontLibrary& fonts();

// Font for a style: TextStyle::fontFile, else fontFamily (installed font), else the bundled Thai
// font. `bold` / `italic` ask for that face when one is installed; otherwise the regular face is
// used (italic is then synthesised by shearing, bold is not synthesised).
// FontId{0} is a valid id; this value means no font could be loaded at all.
inline constexpr font::FontId kNoFont{0xFFFFFFFFu};
font::FontId fontForStyle(const TextStyle* style, bool bold = false, bool italic = false);
// The style a text entity uses: its own, else the document's current one, else Standard.
const TextStyle* styleFor(const Document& doc, TextStyleId id);

// ---- grapheme clusters -----------------------------------------------------------------------

// Byte offsets of cluster starts, including 0 and utf8.size() (so n clusters give n+1 offsets;
// empty text gives {0}). A cluster is a base character plus its combining marks (Thai vowels and
// tone marks above and below, SARA AM, ZWJ / ZWNJ, variation selectors) and a Thai leading
// vowel (U+0E40..U+0E44) belongs with the consonant that follows it. Invalid UTF-8 bytes count
// as one cluster each.
std::vector<std::size_t> clusterBoundaries(std::string_view utf8);

// ---- markup ------------------------------------------------------------------------------------

// Formatting in force for a piece of MTEXT.
struct Attr {
    bool bold = false;
    bool italic = false;
    double height = 0;  // mm, capital height (resolved against the entity's height)
    int color = -1;     // palette index, -1 = the entity's colour
    friend bool operator==(const Attr&, const Attr&) = default;
};

struct Piece {
    std::string text{};  // never contains a newline
    Attr attr{};
    bool newline = false;  // paragraph break (text is empty)
};

// Parses TBCAD markup: `\P` paragraph, `\\ \{ \}` literals, `{\b1;..}` `{\i1;..}` bold/italic,
// `{\H<h>;..}` height in mm (`{\H2x;..}` = relative to the surrounding height), `{\C<i>;..}`
// palette colour. Anything else after a backslash is kept literally; unmatched braces are
// ignored. `baseHeight` is the entity's text height.
std::vector<Piece> parseMarkup(std::string_view markup, double baseHeight);
// Plain text for display in an editor / list: markup removed, paragraphs become '\n'.
std::string plainText(std::string_view markup);
// Escapes plain text (with '\n' paragraph breaks) into markup.
std::string markupFromPlain(std::string_view plain);

// ---- layout ------------------------------------------------------------------------------------

struct PlacedGlyph {
    font::FontId font{};
    std::uint32_t glyphId = 0;
    Vec2 origin{};       // glyph origin in layout space (mm), y up, includes the shaper's offsets
    double scale = 0;    // mm per em
    double shear = 0;    // synthetic italic: x += shear * y (in glyph space)
    int color = -1;      // palette index or -1
};

struct LayoutLine {
    double baseline = 0;  // y in layout space
    double x0 = 0;        // left edge of the line
    double width = 0;     // advance width, trailing spaces excluded
    double height = 0;    // largest capital height on the line
    std::string text{};   // the line's text (trailing spaces dropped), for tests and editors
    std::size_t firstGlyph = 0, endGlyph = 0;  // range in TextLayout::glyphs
};

// A laid-out text. Layout space has its origin at the entity's alignment point, x along the
// baseline direction and y up; `toWorld` maps it to the drawing (rotation, width factor, oblique
// angle, Aligned / Fit scaling and the insertion point).
struct TextLayout {
    std::vector<PlacedGlyph> glyphs{};
    std::vector<LayoutLine> lines{};
    BBox2 frame{};  // nominal box (line advances x capital heights, descender allowed below)
    BBox2 box{};    // frame united with the glyph outlines (marks above the line included)
    Transform2 toWorld{};

    bool empty() const { return glyphs.empty(); }
    // The four corners of `frame` in the drawing, counter-clockwise from its bottom-left.
    std::array<Vec2, 4> frameCorners() const;
    // Bounding box of `box` in the drawing.
    BBox2 worldBounds() const;
};

TextLayout layoutText(const Document& doc, const TextData& t);
TextLayout layoutMText(const Document& doc, const MTextData& t);
// Layout of either kind; empty for other kinds.
TextLayout layoutEntity(const Document& doc, const Entity& e);

// Advance width in mm of a single line of plain text at the given capital height.
double measureWidth(const TextStyle* style, std::string_view utf8, double height,
                    bool bold = false, bool italic = false);

// Fraction of a line's height used for line pitch in MTEXT (5/3, like AutoCAD).
inline constexpr double kLineSpacing = 5.0 / 3.0;
// Shear used for synthetic italics (12 degrees).
double syntheticItalicShear();

// ---- display / bounds / hit testing --------------------------------------------------------------

// Registers the Text and MText generators and bounds with geom::DisplayRegistry. Idempotent; call
// at start-up before concurrent use.
void registerDisplay();

// Filled glyph rings (one DisplayFill per glyph); a closed box per line when the capital height
// is below `minPixels` pixels (DisplayContext::pixelSizeMm).
inline constexpr double kPlaceholderPixels = 3.0;
void buildTextDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                      DisplayList& out);

// True when p lies within the text's frame grown by `aperture`.
bool hitTest(const Document& doc, const Entity& e, Vec2 p, double aperture);

}  // namespace tbcad::text
