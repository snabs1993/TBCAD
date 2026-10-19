# Performance guards on big drawings

Numbers measured on a 4-core Linux container, GCC Release build (`build/linux-gcc-release`),
2026-10-19. They are for comparing builds, not promises: a desktop CPU is faster.

## Editing commands (libs/e2e/tests/edit_family_perf_tests.cpp)

Run: `TBCAD_BENCH_N=196000 tbcad_e2e_tests -tc="perf: editing commands*"` (196000 pairs of
polyline + line = 392000 objects; the mixed drawing adds SHX text, MTEXT, circles, blocks with
attributes and solid hatches sent to back, 456680 objects). `TBCAD_BENCH_ONLY=LABEL` runs one
command for a profiler. The test fails when the mix costs more than 4x plain geometry plus slack.

Milliseconds, plain / mixed:

| Command | run | undo | redo |
|---|---|---|---|
| COPY | 486 / 685 | 248 / 349 | 302 / 621 |
| MIRROR | 440 / 736 | 233 / 321 | 304 / 569 |
| ARRAYRECT | 122 / 340 | | |
| EXPLODE | 1751 / 3455 | 877 / 1315 | 1299 / 1731 |
| CHPROP Color | 699 / 984 | 272 / 616 | 332 / 615 |
| CHPROP Layer | 593 / 1085 | 248 / 632 | 263 / 597 |
| LAYER Off | 67 / 118 | 1 / 6 | 0 / 1 |
| LAYER Lock | 148 / 379 | 104 / 259 | 104 / 238 |
| MATCHPROP (200 picks) | 30 / 46 | 16 / 23 | 16 / 22 |
| MOVE | 485 / 968 | 251 / 630 | 250 / 643 |
| ROTATE | 517 / 992 | 235 / 667 | 280 / 613 |
| SCALE | 548 / 1078 | 254 / 617 | 227 / 695 |
| STRETCH | 861 / 1822 | 287 / 675 | 270 / 738 |
| ERASE | 326 / 430 | 216 / 795 | 117 / 199 |
| PURGE | 59 / 174 | 28 / 116 | 24 / 193 |
| AUDIT | 19 / 27 | | |

Only EXPLODE (a real 400k-object rewrite) is over 2 s. Every other command on the mixed drawing
stays under 1.1 s for run and under 0.8 s for undo and redo, within 2.1x of plain geometry.
Debug builds are 5 to 10 times slower; do not read the absolute numbers off a Debug run.

## Object snap (libs/drafting/tests/osnap_perf_tests.cpp)

393000 objects (lines, circles, arcs, bulged polylines, ellipses, splines, hatches), all snap
modes, 300 random mouse moves, Release:

| Drawing | mean per move | worst |
|---|---|---|
| spread out, 2 km | 0.11 ms | 22 ms (first, cold move) |
| survey coordinates 1e6, 2 km | 0.02 ms | 0.3 ms |
| dense, 200 m, aperture 100 mm/px | 4.5 ms | 8.6 ms |
| dense, 200 m, aperture 1000 mm/px | 5.7 ms | 10.9 ms |
| survey 1e9, dense | 6.2 ms | 13 ms |

The dense cases put 393000 objects in 200 m, so 65 snap candidates fall in one aperture; real
drawings are far sparser. Before this work the same dense drawing took 120 ms per move because
every ellipse, spline and hatch whose bounding box held the cursor was intersected over its whole
length.
