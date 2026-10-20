# Text (W4-4)

Libraries: `libs/text` (`tbcad::text`: layout, wrapping, display) and `libs/commands/text`
(module `textcmd`: TEXT, MTEXT, STYLE, TEXTEDIT and the editor panel). Fonts come from `libs/font`
(FreeType + HarfBuzz); the bundled Sarabun covers Thai and Latin.

## Sizes

Text height means **capital height**: the layout scale is `height / capHeight(font)`, so 2.5 mm
text has 2.5 mm capitals. A style's width factor and oblique angle combine with the entity's own
(`widthFactor = style * entity`, `oblique = style + entity`).

## TEXT alignment

`TextData::position` is the insertion point. For every alignment except Left/Baseline the
alignment point is `alignPoint` (the DXF importer stores it that way; the TEXT command sets both
points equal for Left/Baseline). Vertical: Baseline, Bottom (the descender line sits on the
point), Middle (half a capital height below the cap line), Top. `Middle` (horizontal) centres on
both axes. **Aligned** and **Fit** run the baseline from `position` to `alignPoint`: Aligned
scales width and height together, Fit keeps the height and changes the width factor. The entity's
rotation is ignored for those two.

## MTEXT

* Markup: `\P` new paragraph, `\\ \{ \}` literals, `{\b1;..}` bold, `{\i1;..}` italic,
  `{\H<h>;..}` height in mm (`{\H2x;..}` relative to the surrounding height), `{\C<i>;..}` palette
  colour, `\S<num>/<den>;` a stacked fraction (`#` for a diagonal bar, `^` for none, as in
  tolerances) set at 70% of the text height. A raw newline counts as `\P`. Anything else after a backslash is kept as typed; unmatched braces are ignored.
  `text::markupFromPlain` / `plainText` convert to and from plain text.
* Bold and italic use an installed bold/italic face of the style's family when there is one. With
  only the bundled regular face, italic is synthesised (12° shear) and bold is not.
* Line pitch is 5/3 of the line's capital height (`kLineSpacing`); the first baseline is one
  capital height below the top of the frame. Lines are left aligned; `attach` chooses which point
  of the frame sits at `position`. `width = 0` means no wrapping.

### Character codes

`\W<f>;` glyph width factor (relative to the style's), `\T<f>;` tracking (letter spacing, 1 =
normal), `\Q<deg>;` extra slant, `\A0|1|2;` where smaller text sits on a line of taller text
(bottom, middle, top), `\K` / `\k` strike-through on / off. They follow braces like `\b` and `\H`.
Out of range values are ignored. DXF keeps them (a non-numeric argument is dropped and counted).

### Paragraphs

`\pxi-3,l6,r2,qj,t12,24,sb1,sa2;` sets the paragraph settings until the next `\p`: `i` first-line
indent (relative to `l`), `l` left indent, `r` right indent, `q` alignment (`l c r j d` = left,
centre, right, justify, distribute), `t` tab stops (comma list), `sb`/`sa` space before/after.
Justify stretches the spaces and Thai word gaps of every line but the last of a paragraph;
distribute stretches the last line too. A tab jumps to the next stop (default pitch 4 text
heights). `\~` is a no-break space, `\N` forces the next column. `MTextData::lineSpacing`
(0.25 to 4, default 1) multiplies the line pitch. Indents and tab stops are drawing units, so a style width factor does not change where they fall. DXF: `\p` (lengths scaled to mm), `\N`, group
44 with 73 = 1; .tbcad: optional f64 after the column tail.

### Background mask

`MTextData::bgMode` 1 draws a rectangle behind the text in the drawing background colour (it hides
what lies behind, like a wipeout, also in plots); 2 fills it with palette colour `bgColor`. The
rectangle is the text frame grown by `(bgScale - 1)` text heights on every side (`bgScale` 1 to 5,
default 1.5). DXF groups 90 (1 colour, 2/3 background), 63, 45; .tbcad: tail after the line spacing.

### Columns

`MTextData::columns` (2 to 8) with a `width` flows the text through that many columns, each
`width` wide and `columnGap` apart (0 means twice the text height). The lines are split so the
columns end at about the same height; every column starts at the top of the frame. In the MTEXT
command the Columns option turns the rectangle you draw into the whole block: it is cut into
columns with a gap of two text heights. The setting is saved in .tbcad files (an extra tail on the
record, ignored by older readers) and written to and read from DXF (groups 75, 76, 79, 48, 49).

### Wrapping and Thai

Break opportunities are spaces, the zero-width space U+200B (not drawn), and, inside Thai text,
the word boundaries found with a dictionary:

* `assets/dictionaries/thai-words.txt` is a CC0 word list (60,958 words) derived from PyThaiNLP's
  `words_th.txt`; see `assets/dictionaries/README.md` and `third_party/notices/thai-wordlist.txt`.
  It is not derived from libthai or ICU. One word per line; edit the file to add words.
* `thaiWordBreaks` splits each Thai run by maximal matching: dynamic programming over grapheme
  clusters, fewest unknown clusters first, then fewest words. Unknown stretches (names, loanwords
  missing from the list) stay together; a break is only offered next to a known word. A repetition
  mark (ๆ) or abbreviation sign (ฯ) never starts a line.
* The list loads on first use from `setDictionaryDirectory` (the app points it at its `assets`
  folder), then the source tree, then `./assets/dictionaries`. Without a list, Thai wraps at
  cluster boundaries only (below).

A run that is still wider than the whole width (one very long word, or Thai text with no list) is
filled **grapheme cluster by cluster**. A cluster is a base character with its combining marks
(Thai vowels and tone marks above and below, SARA AM, ZWJ/ZWNJ) plus a Thai leading vowel
(U+0E40..0E44) with the consonant that follows it. A cluster is never split. A word wider than the
width starts on a new line first.

Limits: the list is a plain word list without frequencies, so rare ambiguous phrases can segment
differently than a person would; a word missing from the list may be split at a dictionary word
inside it. Use U+200B to force a break opportunity.

## Display

`registerDisplay()` (called by the module's `registerCommands`) installs the generators for
`EntityKind::Text` and `MText` in `geom::DisplayRegistry`:

* one `DisplayFill` per glyph, holding all of its contours (even-odd), colour from the entity's
  resolved style or from `\C` overrides; outline flattening tolerance follows the zoom
  (`DisplayContext::tolerance()`, quantised to powers of two so the font library's per-tolerance
  outline cache stays small);
* when the capital height is below 3 pixels (`kPlaceholderPixels`) one closed box per line is
  drawn instead;
* bounds are the transformed layout box united with the glyph outlines, so Thai marks above the
  capital line are included; `text::hitTest` tests the layout frame (grown by the aperture).

Glyph outlines are cached in em space by `libs/font` per (font, glyph, tolerance). Triangulation
is done by the renderer from the rings.

## Commands

| Command | Aliases | Notes |
|---|---|---|
| `TEXT` | `T`, `DT` | start point `[Justify/Style]`, height, rotation, text. Height and justification are remembered |
| `MTEXT` | `MT` | first corner `[Height/Justify/Rotation/Style/Width]`, opposite corner (rectangle preview), text (type `\P` for a new line) |
| `TXT2MTXT` | | select single-line and multiline text; they are merged top to bottom into one multiline text and the originals are erased. **SEttings** (at the selection prompt) chooses the **Order** (top down, or the order you selected them) and **Wrap** (one paragraph wrapped at the widest text instead of one line each); both are remembered |
| `TCOUNT` | | select single-line text in the order you want, then give a start number, an increment, a prefix and a suffix; each text is replaced by its number |
| `MTEXTFORMAT` | `MTF` | select MTEXT; Background (None / Window / Colour, then the border factor) or LineSpacing. The Text panel has buttons for underline, strike, width, paragraph alignment, indent and tab, and a shortcut to this command |
| `SPELL` | | select text; each English word that is not in the dictionary asks Ignore / Change / Add / All, plus Suggest (takes the nearest known word) when there is one. Words with Thai or other non-ASCII letters, digits or all capitals are never flagged. Added words are saved in the drawing |
| `STYLE` | `ST`, `TEXTSTYLES` | opens the **Text Styles** dialog (see below) |
| `-TEXTSTYLE` | | the typed form: name (`?` lists), font family or `.ttf/.otf` file, width factor, oblique angle, fixed height (0 = variable); makes it current |
| `TEXTEDIT` | `ED`, `DDEDIT` | selected text, new text (edited in place in a box at the text; the old text is the default). **Mode** Single / Multiple; with `TEXTEDITMODE` 0 (Multiple, the default) it asks for the next object until Enter, and **Undo** takes the last edit back. An object selected first (a double-click, the Text panel) is always a single edit. The whole command is one undo step |

## The text editors

MTEXT and TEXTEDIT on multiline text edit in a box at the text, with a formatting bar above it.
The **Text** panel (View > panels) has the same bar for the selected object; Apply runs `TEXTEDIT`.
Both use the ordinary ImGui text input, so the Windows IME (Thai) works as it does in the command
line. The bar (`textcmd::FormatBar`, edits in `format_edits.h`):

* **B I U O S**: wrap the selected text in bold, italic, underline, overline or strikethrough
  markup; **a/b** stacks a selected `1/2`, `1#2` or `1^2` (`\S1/2;`).
* **Colour** (ACI 1 to 9 or a number) and **Set height** for the selected text.
* **@ Symbol**: degree, plus/minus, diameter, almost equal, angle, delta, identity, not equal, ohm,
  centre line, property line, squared, cubed, subscript 2, non-breaking space, Baht and the Thai
  marks ๆ and ฯ.
* **Left / Centre / Right / Justify** for the paragraph at the cursor.
* **Bullets and numbering**: bulleted, numbered (1. 2. 3.) or lettered (a. b. c.) lists with a
  hanging indent of two text heights, or Off; the paragraph after a list starts without the indent.
* **Paragraph...**: first line indent, left and right indents, tab stops, space before and after
  and alignment, as one `\px...;` code at the start of the paragraph (it holds until the next).
* **Find and replace** inside the text, with a live count and Match case.
* **Spelling**: the English words SPELL would flag, each with its suggestions to click.

MTEXT's box also has Height, Justify and Style, **Columns** (none; 2 to 4 dynamic, balanced to end
at about the same height; or 2 to 4 static, each as tall as the rectangle or the **Column height**
typed) and **Mask** (background mask in the drawing background colour; MTEXTFORMAT sets a fill
colour and the border). Static columns are stored as `MTextData::columnHeight` (DXF 75 = 1 with
the height in 46; a tail of the `.tbcad` MTEXT record that older readers ignore).

## Fixed text height

A text style has a height like AutoCAD's STYLE height (`TextStyle::height`). 0 means variable: TEXT
asks for the height and MTEXT starts from the size the drawing needs (annotation scale, the view,
the size of the drawing). Above 0, every TEXT in the style gets that height without asking, and
MTEXT too unless the Height option is used. Set it with STYLE, `-STYLE` Edit Height, or the text
style panel. It is read from and written to DXF (group 40 of the STYLE record; DWG goes through the
same reader). In `.tbcad` it is a separate `TSTH` chunk (format minor 11) that programs which do not
know it keep unchanged; collaboration change records are version 3 (version 2 is still read).

**TEXTALIGN** lines several texts (single-line or multiline) up with the first one selected: by Left, Center, Right, Top, Middle or Bottom edge of their bounding boxes, or, as in AutoCAD, along a line from the first text's lower left corner through a picked point: **Points** slides each text onto the line, **Distribute** spreads them evenly up to the point and **Spacing** puts them a given distance apart, in their order along the line. One undo step.

## ARCTEXT

**ARCTEXT** puts text along an arc or circle. Pick the arc, type the text, give the height and choose
the side: **Outside** (the default; reads left to right along the top, letters stand on the arc with their
tops away from the centre) or **Inside** (letters' tops point to the centre; on a circle the text sits at the
bottom). The text is centred on the middle of the arc. Each letter is its own text object, so the result is
not linked to the arc and is edited letter by letter; one undo step removes it all.

`MTEDIT` is another name for `TEXTEDIT`.

## Text Styles dialog

`STYLE` (Format > Text Style Manager) lists the styles with a preview. Every change runs `-STYLE`,
so it is one undo step. The font box takes a family or a file; the list beside it offers the
bundled Sarabun, the stroke font and the fonts installed on the computer, filtered by what you
type. Under it the dialog says whether the font has Thai letters; Thai text in a font without them
is drawn with Sarabun. **Effects** sets Annotative, Backwards and Upside down, the same as
`-STYLE Edit <name> Annotative|Backwards|UpsideDown Yes|No`.
