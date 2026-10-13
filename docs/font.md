# libs/font: fonts and Thai text shaping

`tbcad::font` loads TrueType/OpenType fonts, shapes UTF-8 text with HarfBuzz
(correct Thai mark stacking and sara am handling) and returns glyph outlines as
flattened polylines. It has no dependency on `libs/core`, uses its own small
`Result<T>` and `FPoint`, and is thread-safe.

```cpp
#include "tbcad/font/font_library.h"
using namespace tbcad::font;

FontLibrary lib;
auto latin = lib.loadBundled("Lato");        // Result<FontId>
if (!latin.ok()) { /* latin.error().message */ }

// "Room " comes from Lato, the Thai part falls back to the bundled Sarabun.
for (const ShapedRun& run : lib.shape(latin.value(), "Room ห้องนอน")) {
    double pen = 0;
    for (const ShapedGlyph& g : run.glyphs) {
        const GlyphOutline& o = lib.outline(run.font, g.glyphId);   // em units
        // glyph origin = pen + g.xOffset / run.unitsPerEm, g.yOffset likewise
        pen += double(g.xAdvance) / run.unitsPerEm;
        // ... o.contours: closed polylines (last point is not repeated)
    }
}
```

## API summary

| Call | Notes |
| --- | --- |
| `load(path)`, `loadFromMemory(bytes, faceIndex)` | Face 0 by default. Corrupt or unreadable data gives an error, never a crash. The same path returns the same `FontId`. |
| `loadBundled("Sarabun" \| "Lato")` | Looks in the bundled directory (see below). |
| `setBundledDirectory(dir)` | Defaults to the source tree's `assets/fonts` (compile-time path), then `./assets/fonts`. The installer (W6-1) must set the shipped location. |
| `findSystem(family, bold, italic)` | Scans `C:\Windows\Fonts` (Windows) or `/usr/share/fonts` and `/usr/local/share/fonts` (Linux) once, reading name tables with FreeType; no Win32 font API. Case-insensitive family match, prefers an exact bold/italic match. |
| `shape(font, utf8, direction)` | Returns a `std::vector<ShapedRun>` (see below). |
| `outline(font, glyphId, tol = 1/2000)` | Contours in em units, y up. Cached per (font, glyph, tolerance); the reference stays valid for the library's lifetime. |
| `metrics(font)` | Ascender, descender, capHeight (OS/2, fallback 0.7 em), xHeight (OS/2, fallback 0.5 em), all in em units. |
| `hasGlyph(font, cp)`, `familyName(font)` | Helpers. |

### Shaping and fallback

`shape` picks, for every code point, the first font of the chain *requested
font → bundled Sarabun → bundled Lato* that has a glyph. Combining marks,
joiners and spaces stay with the previous character's font when that font has
them. Consecutive characters from one font form one `ShapedRun`, so mixed text
returns several runs in logical order (reversed for `Direction::RTL`, so they
come out in visual order). Advances and offsets are font units
(`ShapedRun::unitsPerEm`); `cluster` is the byte offset into the input string.

Sarabun places many Thai marks by substituting pre-stacked glyphs (zero offset)
and others by GPOS (non-zero offsets); both are handled by HarfBuzz, callers
just add `xOffset`/`yOffset` to the pen position.

Invalid UTF-8 never crashes: bad bytes become U+FFFD.

## Bundled fonts

Both are SIL OFL 1.1; licence texts are in `third_party/notices/fonts-*.txt`.

* `assets/fonts/Sarabun-Regular.ttf`: Thai + Latin.
* `assets/fonts/Lato-Regular.ttf`: Latin only (also exercises the fallback path).

No Windows system font is bundled.

## Dependencies and licences

* FreeType 2.13.3 under the **FTL** (`cmake/deps/freetype.cmake`), outlines only
  (zlib, bzip2, png, brotli, harfbuzz disabled). FTL requires this credit in the
  product documentation (W3-1 puts it in Help > About): *Portions of this
  software are copyright (c) The FreeType Project (www.freetype.org).*
* HarfBuzz 10.1.0 ("old MIT"), compiled from the amalgamated `src/harfbuzz.cc`
  (`cmake/deps/harfbuzz.cmake`). Glyph outlines come from FreeType, so HarfBuzz's
  own FreeType/GLib/ICU integrations are not built.

Both are fetched with `FetchContent`, pinned to an exact URL and SHA-256.

## Known limits

* Thai word-break is not done here (W4-4 documents it): the library only shapes.
* Variable-font axes are not exposed; the default instance is used.
* `shape` takes one lock for the whole call, so threads shaping at the same time
  are serialised.

## Single-stroke fonts (`.tbsf`)

TBCAD ships its own single-stroke font, **TBCAD Stroke** (`assets/fonts/stroke/tbcad-stroke.tbsf`),
designed by the TBCAD contributors (MIT). It is the replacement for AutoCAD's SHX fonts: text in it
is drawn as open polylines, not filled outlines, so it renders quickly and plots as plain lines.
Text height is the capital height, like every other font.

Use it by naming the family `TBCAD Stroke` (case-insensitive) in a text style, or by pointing a
style's font file at any `.tbsf` file. Characters the stroke font lacks fall back to the outline
fonts. DXF import maps every text style whose font file ends in `.shx` to `TBCAD Stroke` and adds
an info message (`dxf.reader.shx_font_mapped`) to the import report; big fonts are dropped.

File format (UTF-8 text, `#` starts a comment, unknown keys are ignored):

```
tbsf 1
family TBCAD Stroke      # default "TBCAD Stroke"
upm 20                   # design units per em; every coordinate is divided by it
cap 14  xheight 10  ascender 14  descender -4
tonedrop 1               # Thai: how far a tone mark drops when no upper vowel is under it
glyph 41 12              # hex code point, advance
s 0 0 5 14 10 0          # one stroke: x y pairs
s 2 5 8 5
glyph E48 0 mark=tone    # Thai mark: zero advance, centred over the previous base glyph
s ...
```

API: `FontLibrary::loadStroke(path)`, `loadStrokeFromMemory(text)`, `loadBundledStroke()`,
`isStroke(id)` and `strokes(id, glyphId)` (the glyph id is the code point). `shape()` works on
stroke fonts like on any other font; `outline()` is empty for them. The bundled font covers ASCII, common CAD symbols (° ± × ÷ ² ³ µ Ø ø Δ Φ Ω ⌀) and the whole Thai
block (consonants, vowels, tone marks, digits, ฿). Thai marks have no GPOS rules in a stroke font:
upper vowels and tone marks are centred over the previous base letter, and a tone mark with no upper
vowel under it drops by the `tonedrop` value so it sits at vowel height. The bundled font is
regenerated with `python3 tools/fonts/make_stroke_font.py` (Latin) and `tools/fonts/thai_glyphs.py`
(Thai); every glyph is original work of the TBCAD contributors.
