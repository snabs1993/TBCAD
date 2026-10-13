# transform: move, copy, rotate, scale, mirror, stretch, grips

Module `libs/commands/transform`. All commands run in one transaction, so each is one undo step.

| Command | Alias | Notes |
| --- | --- | --- |
| MOVE | M | base point, second point (Enter: the base point is the displacement) |
| COPY | CO, CP | repeats until Enter or Exit; **Array** makes a row of copies (see below) |
| ROTATE | RO | angle or Reference; Copy keeps the source |
| SCALE | SC | factor or Reference; Copy keeps the source |
| MIRROR | MI | two points; optionally erase the source |
| STRETCH | S | crossing window, base point, second point |
| GRIPEDIT | | started by clicking a grip of the selection (Ctrl: copy) |

## STRETCH

Pick two corners of a crossing window, then a base point and a second point. Only what lies in
the window moves (`stretchByWindow` in `stretch.h`):

- Lines, arcs, polylines, splines and leaders: the vertices inside the window move, the others stay.
  A polyline segment next to a moved vertex follows; a line with both ends inside simply moves.
- Circles, ellipses, text, MText, inserts and points move whole when their anchor (centre or
  insertion point) is inside the window.
- Dimensions and hatches move whole when their bounding box is inside the window.
- Hidden, frozen and locked layers and entities inside block definitions are ignored.

## COPY > Array

At the second-point prompt type `A` (Array), give the number of items (2 or more, default 5), then
a second point. The point sets the spacing between neighbouring copies; with `Fit` it is the
position of the last copy and the copies are spread evenly up to it. The source stays where it is.
