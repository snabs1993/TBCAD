# libs/render: GPU renderer

OpenGL 3.3 core renderer for 2D drawings. Callers hand it `DisplayList`s (already tessellated and
style-resolved); it keeps them as tile-relative vertex batches and draws them.

```
Document ──(display lists)──▶ Scene ──(batches)──▶ Renderer ──▶ framebuffer
                                         Overlay ──────▶
```

| Piece | What it is | GL needed |
| --- | --- | --- |
| `Camera2D` | centre / pixelsPerMm / viewport, pan, zoom at cursor, fit | no |
| `Scene` | tiles, style batches, culling, layer visibility, highlight/selection geometry | no |
| `Overlay` | immediate-mode previews, grips, snap markers, rubber band, selection window | no |
| `triangulateEvenOdd` | earcut with even-odd nesting for hatch/solid fills | no |
| `Renderer` | uploads batches, draws them, draws the overlay | yes (`src/gl/` only) |
| `syncLayerVisibility` | copies `Document::effective(layer).visible()` to the scene | no |

## Usage

```cpp
Renderer renderer;
renderer.init(reinterpret_cast<GLLoadProc>(glfwGetProcAddress));   // context must be current

Scene scene({.tileSizeMm = Scene::suggestTileSize(documentExtent)});
scene.upsert(entityId, displayList, drawOrder, camera.pixelsPerMm);  // on every document change
syncLayerVisibility(scene, doc);                                     // after layer changes

renderer.setCamera(camera);
renderer.beginFrame();
renderer.draw(scene);       // drawing + highlight + selection glow
renderer.draw(overlay);     // rebuilt every frame
RenderStats stats = renderer.endFrame();
```

Re-tessellate when `Scene::needsRetessellation(oldCam, newCam)` says the zoom changed by more than 4x:
`Scene::staleEntities(camera)` lists only the visible entities built for a different zoom.

## How precision works

World coordinates are doubles. A vertex is stored relative to the origin of its tile as a
(hi, lo) float pair per axis, and the shader receives `tileOrigin - cameraCenter` (computed in double)
split the same way, so the sum is exact near the camera. Measured error at a camera centre of
1e8 mm and 1000 px/mm is below 1e-3 px (`tests/precision_tests.cpp`). Plain float vertices would be
off by thousands of pixels there.

## Limits worth knowing

- Thick lines are one instanced quad per segment, extended by half a width at both ends. Joins of
  very wide polylines at sharp angles look blunt (no miter/round join geometry yet).
- Highlight/selection glow is a translucent wide line; overlapping segments of one polyline darken
  slightly at joins.
- Dash patterns have at most 32 elements; longer ones are drawn solid.
- Fill triangles are tile-relative by their centroid. Fills far larger than a tile can show
  sub-pixel jitter of their edges at extreme zoom (strokes are exact).
- `drawOrder` orders style batches within fills, strokes and points. Inside one batch (same layer,
  colour, lineweight and linetype) the order is insertion order, which only matters for
  overlapping translucent geometry.

## Tests and benchmark

`tbcad_render_tests` has CPU-only tests plus GL smoke tests that draw into an offscreen framebuffer
and read pixels back. They need a context without a window: Mesa llvmpipe through EGL on Linux
(`libegl1 libegl-mesa0 libgl1-mesa-dri`), the GPU driver on Windows. Without one they print
`SKIPPED`; set `TBCAD_REQUIRE_GL=1` to turn that into a failure (CI does).

`render_bench` draws N random lines (default 1,000,000) offscreen and prints frame times for four
camera paths. Run it on a machine with a real GPU:

```
render_bench --lines 1000000 --frames 240 --size 1920x1080 [--thick]
```
