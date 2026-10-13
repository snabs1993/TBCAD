# Underlays (`tbcad::underlay`)

An underlay is a drawing or picture you look at and trace over. In TBCAD an underlay is an
**external reference** (`docs/xref.md`): a block whose path is stored with the drawing, whose
content is a snapshot saved in the file, and which reloads by itself when the file changes. This
page is about PDF pages and pictures.

## PDF pages

`PDFATTACH` (alias `PA`) attaches one page of a PDF as vector geometry:

1. Enter the PDF file.
2. Enter the page number (asked only when the file has more than one page).
3. Enter the fade, 0 to 90 percent (default 50). Colours move toward a neutral grey (160), so the
   underlay stays visible on a dark or a light background but never competes with your drawing.
4. Name it, then place it like `XATTACH` (point, `Scale`, `Rotate`).

`XATTACH` also takes a `.pdf` (page 1, default fade). The stored path carries the options:
`plans/ground.pdf#page=2,fade=70`. `XRELOAD` (and the automatic reload when the file changes) keeps
them. To change the page or the fade, detach and attach again.

What is imported: strokes (lines, rectangles, curves) become polylines in millimetres (1 PDF point =
0.3528 mm, `UserUnit` and page `/Rotate` honoured, the lower-left corner of the page box at the
origin); filled shapes become closed outlines in the fill colour. What is not: text and raster
images (the command prints how many were left out), shading, clipping. White fills and fills that
cover the page are dropped as paper. Plots from CAD programs keep their text as line work, so they
come through whole; a PDF made from a word processor shows its drawings but not its words.

All geometry is on one layer, `PDF`, below the xref's own layer (`site` > `PDF`). Turn the layer
off or freeze it to hide the underlay. **Object snap ignores underlays**, so the cursor does not
jump to a thousand PDF vertices while you trace.

Limits: encrypted PDFs are refused with a message; a page may hold up to 4 million vertices (the
rest is left out and the report says so); Flate, ASCIIHex and ASCII85 streams are read (other
compressions only matter for images, which are not read anyway).

## How the PDF is read

`libs/underlay` has its own small PDF reader (no third-party PDF code). It finds objects by
scanning the file, so a damaged or incrementally updated cross-reference table does not matter; it
reads object streams and the page tree, then runs the content stream (`q Q cm m l c v y h re S s f F
B b n W g G rg RG k K sc scn SC SCN Do BI`) and form XObjects up to 8 levels deep.
`importPdf(bytes, options, report)` returns a `Document`; `PdfReport` says what was left out.

## Pictures

`IMAGEATTACH` (`IAT`) attaches a PNG, JPEG or BMP picture behind the drawing. It asks for the
file, how much to fade it (0 to 95 percent, default 30), a name and the insertion point; the
`Scale` option of the insertion is the size of one pixel in millimetres (default 1).

* The picture is an external reference like a PDF page: the block holds only a closed frame
  polyline (layer `IMAGE`, in pixel units, origin bottom-left) and the path of the file. The pixels
  are never stored in the drawing, so keep the file next to it (a relative path is taken from the
  drawing's folder). It is reloaded by itself when the file changes.
* `IMAGECLIP` (`ICLIP`) shows only a rectangle of the picture: two corners, or `Delete` to show it
  all again. The rest is not drawn, the frame stays whole so the picture can still be picked.
* `UNDERLAYFADE` (`UF`) changes the fade of a picture or a PDF page (0 = as is, 95 = nearly gone;
  a PDF page is limited to 90 and is read again with the new colours).
* Display settings (fade, clip) of each picture live in the drawing header under
  `tbcad.underlays`, one line per block: name, fade, then the clip as fractions of the picture
  (`u0 v0 u1 v1`, `v` counted from the top). Undo covers them.
* Pictures are drawn by the renderer as textured rectangles under the drawing, blended with the
  background, so a faded picture stays out of the way. A picture is decoded once per file and
  time stamp; textures not used for a while are dropped.
* Snapping skips the frame, like PDF underlays. The hide/freeze state of the insert's layer and of
  the layer named like the picture decides whether it is shown.
* Limits: up to 16384 x 16384 pixels and 100 million pixels in all. Pictures are not printed or
  exported by PLOT yet.
