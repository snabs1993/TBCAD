# Practice mode lessons

Practice mode teaches the program with short lessons played in a separate practice drawing. Each
lesson draws a starting drawing, shows faint targets ("ghosts") on the canvas and a list of
steps; the steps tick off by themselves as the drawing and the user's actions meet their checks.

The lessons are data: one JSON file per lesson in `assets/training/lessons/`. The code lives in
`libs/training` (loading, checks, stars, saved progress). `AppCore::addCommandListener` reports
every command start and end, and every undo and redo, together with how the command was started.

## Lesson file

```json
{
  "id": "2.1", "world": 2,
  "title": {"en": "Your first line", "th": "เส้นแรก"},
  "goal": {"en": "...", "th": "..."},
  "tip": {"en": "Shown when the lesson is done", "th": "..."},
  "par": 1,
  "parSeconds": 0,
  "setup": ["ZOOM;W;-200,-500;1200,500"],
  "ghosts": [{"line": [[0, 0], [1000, 0]]}, {"dot": [0, 0]}],
  "steps": [
    {"text": {"en": "...", "th": "..."}, "hint": {"en": "...", "th": "..."},
     "checks": [{"type": "line", "from": [0, 0], "to": [1000, 0]}]}
  ],
  "solution": ["LINE;0,0;@1000,0;"],
  "wrong": [["LINE;0,0;@900,0;"]]
}
```

- `setup`, `solution` and each attempt in `wrong` are script lines as `AppCore::submitScript`
  takes them: parts separated by `;`, an empty part is Enter.
- `par`: the most commands for the third star (ZOOM, PAN, UNDO, REDO and the like do not count).
  `parSeconds` > 0 also needs that time (challenges only).
- Ghosts: `{"line": [[x0,y0],[x1,y1]]}`, `{"circle": [x,y], "radius": r}`, `{"dot": [x,y]}`,
  `{"polyline": [[x,y,bulge]...], "closed": true}` (the bulge is optional).
- A step is done when all its checks pass; a step without checks is done when a later one is.
  Done steps stay done, and a later step that passes ticks off the steps before it, so a user who
  takes another route still finishes. The last step must have checks, and they must hold the
  whole goal: a lesson is complete as soon as its last step passes.

A boss (`"boss": true`, id `B1` ... `B7`) closes its world: one step with the whole goal, the
dashed targets as the picture, and `parSeconds` for the third star. It is listed after the
world's lessons.

## Badges

`libs/training/include/tbcad/training/badges.h` lists the badges and awards them when a lesson is
done (`awardBadges`): finishing given lessons or whole worlds, and counts kept in `Progress` across
lessons (the ways each command was started, lessons where every command was typed, lessons in a
row without Undo).

## Checks

Distances are millimetres (`tol`, default 0.5), angles degrees (`angleTol`, default 0.1).
Object filters, where they apply: `kind` (`line`, `circle`, `arc`, `polyline`, `text`, `mtext`,
`hatch`, `insert`, `dimension`, ...), `layer`, `color` (the colour index shown, ByLayer
resolved). Counts: `equals`, or `min` (default 1) and `max`.

| type | passes when |
|---|---|
| `line` | a line or straight polyline segment matches `from` + `to`, or `from` + `length` / `angle` (or just `length` / `angle`, either direction) |
| `path` | every segment of `path` (and the closing one with `closed`) lies on drawn lines or polyline segments |
| `circle` | a circle with `center` / `radius` |
| `polyline` | a polyline with `closed`, `vertices`, `arcs` (curved segments), `box` (its extents, `[[x0,y0],[x1,y1]]`), `area` (`areaTolPct`, default 0.5) |
| `count` | the number of objects that pass the filters |
| `layer` | layer `name` exists, with `color`, `on`, `frozen`, `locked` |
| `command` | `command` was started (`source`: `button`, `name`, `alias`, `thai`; empty = any) |
| `undo` / `redo` | steps were undone / redone (Ctrl+Z, U, UNDO / Ctrl+Y, REDO) |
| `selection` | the selection has `equals` objects and all pass the filters |
| `view` | `box` is in view and fills at least `minFill` of its width or height |
| `prompt` | `command` is running and its prompt contains `contains` |
| `arc` | an arc with `center` / `radius` |
| `text` | a text, multiline text or leader note (not empty) that contains `contains`, with `height`, starting inside `box`; for a leader, `tip`: a box one of its arrows points into |
| `dimension` | a dimension of kind `dim` (`linear`, `aligned`, `radius`, `diameter`, `angular`) that measures `value` (degrees for `angular`) |
| `object` | an object that passes the filters, whose extents are `box` |
| `block` | a block definition `name` exists, holding `objects` objects |
| `insert` | a placed block `name` at `center`, turned `angle` degrees |
| `saved` | the drawing has a file whose name contains `contains`, with no unsaved changes |
| `space` | the layout tab `name` is shown (empty = Model) |
| `viewport` | layout `name` (empty = any) has a viewport at 1:`value` whose view holds `box` |
| `pdf` | a PDF was written since the lesson started (Export PDF) |

An associative array counts as the objects it shows: a polar array of one circle with 8 items is
8 circles to every check (and its items take the array's layer where they are on layer 0).

## Tests

In `solution` and `wrong` (only there) a few parts stand for what is not typed: `!LINE` picks
LINE from a button (`AppCore::startCommand`), `<Esc>` presses Esc, `<Click x y>` clicks a drawing
point (to pick an object, at a prompt or with no command running) and `<Drag x0 y0 x1 y1>` drags a
selection box between two drawing points (left to right is a window, right to left a crossing).
`<Layout NAME>` picks a layout tab, `<Save>` saves and `<SaveAs NAME>` saves as NAME in a test
folder; `{folder}` anywhere stands for that folder (an Export PDF file name, say). The test window
is not the real one, so Save and Save As typed as commands do nothing there.
A part that is empty after a command has ended is Enter, which repeats the last command.

Settings a command remembers between drawings (the Fillet radius and the like) belong in `setup`
when a lesson depends on them, as the last line: `"FILLET;R;0"` (setup ends any running command).

`tbcad_training_tests` plays every lesson file with the real commands: the starting drawing must
not pass, the solution must pass within par, and every wrong attempt must fail. Add a solution
and at least one wrong attempt to every new lesson.
