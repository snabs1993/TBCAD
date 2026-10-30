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
  takes another route still finishes. The last step must have checks: they are the goal.

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

## Tests

In `solution` and `wrong` (only there) a few parts stand for what is not typed: `!LINE` picks
LINE from a button (`AppCore::startCommand`), `<Esc>` presses Esc and `<Drag x0 y0 x1 y1>` drags a
selection box between two drawing points (left to right is a window, right to left a crossing).

`tbcad_training_tests` plays every lesson file with the real commands: the starting drawing must
not pass, the solution must pass within par, and every wrong attempt must fail. Add a solution
and at least one wrong attempt to every new lesson.
