# Mascot (Clawd)

Source: `https://thesvg.org/icon/claude-code` (`color.svg`, one path, fill `#D97757`, viewBox 24×24). Every edge of the path lies on a 1.5-unit grid starting at (0, 5), so the shape is exactly this 16×10 pixel grid (`#` = filled):

```
      col 0123456789ABCDEF
row 0     ..############..
row 1     ..############..
row 2     ..##.######.##..
row 3     ..##.######.##..
row 4     ################
row 5     ################
row 6     ..############..
row 7     ..############..
row 8     ...#.#....#.#...
row 9     ...#.#....#.#...
```

The grid is the even-odd point-in-path test of the SVG path at each cell centre.

- Body: cols 2–13, rows 0–7.
- Eyes (holes): col 4 and col 11, rows 2–3.
- Claws: cols 0–1 and 14–15, rows 4–5.
- Legs: cols 3, 5, 10, 12, rows 8–9.

Colour `#D97757` on pure black. The display is a round 466×466 AMOLED; the whole drawing, including raised claws and the thinking glyph, stays inside the circle.

## Motion by state

| State | Motion |
|-------|--------|
| IDLE | Slow breathing bob; blink every few seconds; occasional glance; now and then a short sideways scuttle (legs alternate) and back. The whole drawing steps slowly around a lattice to limit AMOLED burn-in; the panel dims and later turns off when nothing happens (`display-power.md`). |
| LISTENING | Claws raised, eyes open taller; a thin ring around the mascot whose width and brightness follow the mic level. |
| THINKING | Legs walk in place; eyes look up; the status bubble shows a spinner cycling through the Claude Code thinking glyphs `· ✢ ✳ ✶ ✻ ✽`. |
| SPEAKING | Body bounce and claw movement driven by the playback level. |
| ERROR | Eyes become crosses for a moment. |
| OFFLINE | Dimmed, eyes closed (one-row lines), slow breathing. |

Motion stays pixel-true: parts move in whole cells (or whole screen-pixel steps that keep cell edges sharp); no blur, fades or anti-aliased scaling of the mascot. The exception is the panel brightness ramp (`display-power.md`): it scales every pixel alike, so it is not a fade of the mascot and no edge changes.

## Status bubble

A pixel-style bubble above the mascot shows the state. IDLE shows no bubble.

| State | Text | Bubble |
|-------|------|--------|
| LISTENING | 在听 | Speech bubble, tail of stepped cells pointing at the mascot; three pixel dots after the text pulse in turn. |
| THINKING | 在想 | Thought bubble: no tail, two small square dots of decreasing size lead to the head; the spinner glyph sits before the text. |
| SPEAKING | 在说 | Speech bubble; three pulsing dots as in LISTENING. |
| ERROR | 出错了 | Speech bubble; text in the error colour. |
| OFFLINE | 离线 | Speech bubble, dimmed together with the mascot. |

- Shape drawn on the same pixel logic as the mascot: stepped corners, outline one bubble-pixel wide. The bubble pops in and out in two or three stepped sizes, never a smooth fade.
- Text uses an OFL-licensed pixel CJK font (Ark Pixel or Fusion Pixel, 12 px design size) rendered 1 bpp at an integer multiple of its design size (48 px), containing only the glyphs above.
- Colours: bubble fill `#F4F3EE`, outline and text `#141413`; error text `#D14343`; OFFLINE fill `#8A877F`. Mascot `#D97757` as above.
- Bubble and mascot together stay inside the 466 circle; the mascot shifts down to make room.

## Sticker layout

A pack clip replaces the pixel mascot with full-frame art. A state with no clip mapped still draws the pixel mascot and its bubble above it, unchanged. Constant classes follow `display-power.md`: M mathematical model, C plain configuration.

- Canvas and reach: the converter's canvas is at most `AP_CANVAS_MAX_PX` (512, C). Each clip records its reach: the distance from the canvas centre to the far corner of its farthest lit pixel, rounded up. A pack is refused unless reach × scale is at most `AP_ART_REACH_MAX_PX` (207, M). The orbit moves the art by up to 17.21 px from the centre, and 207 + 17.21 stays within the 225 px scene bound for every angle, because the reach is a radius. A host test in `apack_test.c` checks the rule against the constants, and the converter applies the same rule.
- Recentred orbit: the art uses the lattice with its mid-point on the scene centre, so the offsets are the raw lattice point plus (2, −10) px (`ORBIT_RECENTRE_X_PX`, `ORBIT_RECENTRE_Y_PX`, derived). That is at most ±10 px in x and ±14 px in y. The pixel mascot keeps the raw lattice. The art never shifts for a bubble.
- Side bubble: the bubble sits beside the art, at the height `bub_y` (screen px from the canvas centre, a multiple of 4, rounded toward zero and limited to ±96) and on the side the clip names. It is 18 units (72 px) tall and 120 to 168 px wide, by its text. Its outer edge `x1` (px from the art centre, a multiple of 4) is the largest value for which the far corner, allowing for the orbit, stays inside the ring's clear radius: `(x1 + 11)² + (|y| + 51)² < 209²` with `y` = `bub_y`. At `bub_y` 0 that is 188; at ±48 it is 172; at ±96 it is 136 (M).
- Pixel placement: the recentred offsets are not multiples of the 4 px unit, so the side bubble is placed in whole pixels from the art centre: its frame, tail or thought dots, spinner, pulsing dots and text rows all come from one pure function (`ag_side_bubble`) and are filled at those pixels, never snapped to the unit grid. The pop steps shrink the bubble about its own centre. Clawd's top bubble is unchanged and takes its rectangle from `ag_top_bubble`.
- Key rule: the art is not written into the scene. Each flush chunk is composed on its way to the panel: a chunk pixel that is exactly 0 and lies inside the art takes the art's colour (`acomp_rows`). Black background, a cleared ring or arc cell, an overlay pixel that rounds to 0 (the ring's faintest edge) and every transparent art pixel are 0 in the output, so the art shows through them. Every other overlay pixel (the ring, the arcs, the bubble and its text) is non-zero and covers the art.
- Area rule: LVGL keeps 32 invalidated areas per frame. The art layer invalidates strips of `DIRTY_BAND_ROWS` rows over the union of the art and the scene. The listening ring (21 areas) is updated in a tick only when the arc change has at most 1 area and the tick's strips (the union of the art and the scene) number at most 10 (`volarc_ring_ok`, M); otherwise the ring waits one tick and catches up in the next. `bands_test` sweeps the bubble box and the art over every orbit point and every angle and checks that the areas of a frame stay within 32. A union above the strip limit invalidates the whole screen.
- Stale art: every tick the strips cover the union of the previous art, the new art and the scene's changed area, so the pixels a moved or ended clip leaves are redrawn as 0 and show the background. Turning the art off (panel off, `fill`) clears its spans the same way. A clear loop writes 0, which now shows the art, so a restore is the invalidation those loops already do.

## Volume arc

- Shown while the volume knob is armed and for `VOL_ARC_LINGER_MS` after it ends, then cleared in one frame. After a cancel it shows the restored value. A knob that arms during the linger takes the arc over without a clear frame.
- Band: pixels whose centre lies between `VOL_ARC_R_IN_PX` (222) and `VOL_ARC_R_OUT_PX` (230) from the centre, written into the rotated output buffer. The inner radius stays above the listening ring's outermost rewritten radius (221) and the outer one below the glass edge (233). If the outer edge is cut off at the glass, setting `VOL_ARC_R_OUT_PX` back to 225 is the only change needed.
- Shape: 270° sweep with the gap at gravity-bottom, starting at the lower left. A pixel is fill when its threshold (1 to 100, one per 2.7°) is at or below the volume, else track. Fill `#F4F3EE`, track `#8A877F`; at 0 % the first three thresholds show in `#D14343`. Hard edges only, no anti-aliasing.
- The arc follows the quadrant target, not the ease: when the quadrant changes it is cleared and drawn once at the new position, also during the linger.
- Invalidation: the arc is invalidated as one area per 10 thresholds (at most 10 for the whole arc, one for a 1 % step). LVGL keeps 32 areas per frame before repainting the screen, so a frame in which the arc invalidates more than one area skips the listening-ring update; the ring catches up in the next frame. A change of the end cue adds one area per changed end; a whole-meter frame folds the lit end cue into the end chunk instead (see End cue).
- End cue: at 0 % and 100 % the arc stretches past its end when the knob keeps turning (see End cue).
- `VOL_ARC_*` values are plain configuration; the table is built at start-up into PSRAM.

## Brightness arc

- Shown while a brightness swipe is armed and for `VOL_ARC_LINGER_MS` after it ends; after a cancel it shows the restored level. No text or icon.
- One rim meter at a time: the volume and brightness arcs share the band, and arming either gesture clears the other meter and draws its own in the same frame.
- Band: the volume arc's band, r 222 to 230. Angles: UI 135° to 225° clockwise from 3 o'clock, the left rim centred on 9 o'clock, so the fill grows from the bottom up.
- Cells: 33, one per ladder level (2.73° each). Cell c (1 to 33) is fill when c ≤ index + 1, else track, so the lowest level still shows one lit cell. Fill `#F4F3EE`, track `#8A877F`, hard edges.
- Frame: the quadrant latched when the touch started, kept while the arc shows; turning the board does not move it.
- End cue: at the lowest and highest level the arc stretches past its end when the swipe keeps pushing (see End cue).
- Invalidation: one area per 9 cells, 4 for the whole arc, one for a one-level step. A frame whose arc change has more than one area skips the listening-ring update, as for the volume arc, so any change of either arc in one frame stays within LVGL's 32 areas; a host test enumerates every change in every quadrant.
- `BRT_ARC_*` values are plain configuration; the table is built at start-up into PSRAM.

## End cue

At either end of either rim arc, travel past the end while the level rests there makes the arc reach past its end by up to `ARC_OVER_CELLS` (3) cells. The level and everything else the gesture reports stay as they are; the cue is drawn only.

- Push: measured in level steps, brightness px / 7.5 (`BRT_TRAVEL_PX / BRT_STEPS`) and knob degrees / 2.7 (`VOL_SWEEP_DEG / 100`). It counts from the moment the level reaches the end, or from arming when the gesture arms there. The peak push is held while the level rests at the end.
- Cells: the number of k in 1 to N with `x · ARC_OVER_C_NUM · (d − k) ≥ k · d · ARC_OVER_C_DEN`, d = N + 1, by integer comparisons, so a huge push gives N and a push of 0 or less gives 0. Cell k appears at x_k = k·d / (c·(d − k)); for c = 1 and N = 3 that is 1.33, 4 and 12 steps, which is 10, 30 and 90 px of brightness travel and 3.6°, 10.8° and 32.4° of knob travel. The arc keeps its in-range rate at the end and each further cell costs more push.
- Hold and release of the push: the stretch holds until the first reversed level step and goes to 0 in the same sample as that level change (one event carrying the level bit and `VK_EV_OVER`), so one frame shows both. Travel back that does not yet change the level changes nothing, so finger noise does not shake the stretch.
- Spring: after release or a lost touch the stretch springs back from the drawn count, the first step at once and then one cell per `ARC_SPRING_STEP_MS`; it ends within `(ARC_OVER_CELLS − 1) · ARC_SPRING_STEP_MS` plus one frame, well inside the linger. The spring starts from the drawn count, so a stretch that was never drawn does not appear after release. A cancel by jump, a new arming and the other meter taking over set it to 0.
- Colour: a stretch cell takes the class of the arc's end cell at that end: fill at the volume maximum and at both brightness ends (the lit run grows from one cell), the red of the mute mark at volume 0. Unlit stretch cells are black; there is no track outside the arc.
- Round ends: the meter's two outer ends and the tip of a stretch are rounded: a half-disc of half the band width on the band's middle radius closes each. The resting cap belongs to the meter table (end cell's colour); the tip cap at stretch k belongs to the stretch table with threshold k at the low end and `ARC_OVER_CELLS` + k at the high end, so it is lit only while the stretch has k cells. The boundary between filled and unfilled meter stays a straight cut. Asserted in `volarc_test.c` C1 to C5.
- Geometry: cells continue each arc's own grid (2.7° per cell on the volume arc, 2.73° on the brightness arc) in a separate table per arc, `VOLARC_OVER_CAP` entries, built at start-up into PSRAM. Cells 1 to N lie below the arc's start and N + 1 to 2N above its end, cell 1 and cell N + 1 touching the end. Both runs fit the 90° gap of the volume arc. The brightness arc's high-end cells (225° to 233°) lie on pixels of the volume arc's table, so the old stretch is cleared in the clear step, before any draw.
- Quadrant: the brightness stretch stays at the latched quadrant; the volume stretch follows the target and re-anchors with the arc.
- Areas: a stretch change is one area per changed end (chunk `ARC_OVER_CELLS`). A whole-meter frame (re-anchor, takeover, clear) folds the lit stretch cells' rectangle into the rectangle of the end chunk they touch (the chunk holding threshold 1 for the low end, threshold `steps` for the high end) and returns the chunk count alone. Limits, asserted in `volarc_test.c` A12 over every pair of drawn states (a sample of levels with every end cue, in all four quadrants, replayed pixel by pixel; then every level of both meters in quadrant 0, areas only): a frame that skips the listening ring spends at most `2 · VOLARC_MAX_AREAS` (20) arc areas, a frame that updates the ring at most `VOLARC_RING_MAX_AREAS` (1), and no frame exceeds 32 with the scene's 10 and the ring's 21. A16 asserts that every changed pixel of every replayed frame lies inside that frame's invalidated areas.
- Test aid: the DEBUG key `arc_over` stretches the forced arc (`arc` or `brt_arc`) by −3 to 3 cells; the forced level must rest at the end the sign names, else nothing shows.

| Value | Class |
|---|---|
| `ARC_OVER_CELLS` 3 | plain configuration |
| `ARC_OVER_C_NUM`, `ARC_OVER_C_DEN` 1 / 1 | mathematical model, placeholder fitted on the board from the `ov=` counters |
| `ARC_SPRING_STEP_MS` 60 | plain configuration |
| `ARC_OV_LOG_MAX` 999999 | plain configuration (log cap) |

## Screen orientation

The QMI8658 IMU keeps the mascot and bubble upright relative to gravity at any angle. The screen is round, so every angle fits.

- Angle: `atan2` of gravity in the screen plane, from the accelerometer at 50 Hz with a low-pass filter. The whole UI (mascot, bubble, ring) rotates as one group around the screen centre. The volume knob reads touch points in the panel frame and never subtracts the UI angle, so clockwise on the glass is clockwise at every orientation. The brightness swipe is the one gesture that uses the UI angle: it reads travel along the UI's up direction at the quadrant target latched when the touch starts. A centre touch must start within 150 px of the centre; a touch starting beyond that is an edge touch (see `protocol.md`, Input).
- All scene content stays within 225 px of the centre, so no angle pushes it off the screen. It also stays at least 2 px inside the volume arc's inner radius (`VOL_ARC_R_IN_PX`, 222 px); a host test checks both bounds against the constants. Both rim arcs (radius 222 to 230 px) are drawn in panel coordinates.
- Ignore changes smaller than 3°, and ease towards the new angle with a 150 ms time constant, so a hand-held board does not jitter.
- Within 5° of 0°, 90°, 180° or 270°, snap to that angle: there the block edges line up with the panel's square pixel grid and stay perfectly sharp. At other angles each block edge gets a one-pixel anti-aliased border.
- Flat on a desk (in-plane gravity below 0.5 g): keep the last angle.
- Quadrant mode (`ORIENT_FREE_ANGLE` 0, the default): the angle changes in 90° steps (switch 15° past a quadrant boundary after 500 ms). A step turns the whole group to the new right angle by the shorter way with a cubic ease-out over `ROTATE_EASE_MS` (plain configuration) and ends exactly on 0°, 90°, 180° or 270°. A new target during a turn restarts from the angle shown. While the panel is off the angle is taken at once.
- Performance gate: at least 25 fps while rotated and no audio underruns. If LVGL's layer transform is too slow, draw the blocks as rotated polygons; if that is still too slow, fall back to 90° steps (switch 15° past a quadrant boundary after 500 ms).
- The IMU-to-screen axis mapping is a board fact: take it from the BSP or Waveshare's examples, log the raw axes and the chosen angle as LOG frames, and have a person confirm by turning the board.
- Mechanism class: mathematical model (gravity angle with low-pass filter, dead zone and right-angle snap). The numbers above are plain configuration constants.
