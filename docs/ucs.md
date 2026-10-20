# User coordinate systems and the adaptive grid

Code: `libs/drafting` (`ucs.h`, `grid.h`, `filters.h`), commands in `libs/commands/drafting`.

## UCS

A UCS is an origin and a rotation. Typed coordinates (`x,y`, `@dx,dy`, `d<angle`), ortho / polar
tracking and the grid all work in the current UCS. Picked points are always stored in world
coordinates; only the way they are entered and constrained changes.

`UCS` (menu Tools / Drafting Aids / UCS...) options:

| Option | Meaning |
| --- | --- |
| World | back to the world system |
| Origin | move the origin to a picked point |
| Rotate | turn the axes by an angle |
| Object | align to a line (origin at its start, X along it), a polyline segment, or the centre of an arc / circle |
| Points | origin point and a point on the X axis |
| Z | same as Rotate (the rotation about Z) |
| 3point, Previous, View | origin and X and Y points; the system before; aligned to the screen |
| Save, Restore, Delete, List | named systems, stored in the drawing (`Header::custom["tbcad.ucs"]`), so they are saved in the `.tbcad` file |
| NAmed | sub-menu Restore / Save / Delete / Rename / List |

The current system is the host setting `drafting.ucs`: a saved name, `World`, or `@x,y,degrees` for an unnamed one.

`UCSMAN` (aliases UC, DDUCS, menu Tools / Named UCS...) opens the UCS dialog: the named systems (set
current, rename, delete, save the unnamed one) and the icon settings. `UCSICON` takes ON, OFF, ORigin,
Noorigin and Properties (icon size 5-95, setting `drafting.ucsicon.size`, and colour Default or 1-255,
`drafting.ucsicon.color`). `UCSFOLLOW` 1 centres the view on the origin of each new UCS; `PLAN` shows the
current UCS, a named one or World. UCSFOLLOW and PLAN changes go in the ZOOM Previous history.

## Dynamic UCS

`DYNUCS` (menu Tools / Drafting Aids / Dynamic UCS) makes ortho, polar and grid snap follow the
straight edge under the cursor (a line or a straight polyline segment): the X axis runs along the
edge and the origin is its start point. It never moves the point itself.

## Adaptive grid

The grid draws two levels. The fine level (spacing times a power of ten, never finer than 8 px)
fades in between 8 and 24 px; the coarse level is ten times wider and always visible. When the fine
level reaches 8 px it becomes the old coarse level, which was already fully visible, so the grid
never jumps. The grid follows the UCS origin and rotation.
