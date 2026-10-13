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
