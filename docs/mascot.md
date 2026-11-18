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

## Volume arc

- Shown while the volume knob is armed and for `VOL_ARC_LINGER_MS` after it ends, then cleared in one frame. After a cancel it shows the restored value. A knob that arms during the linger takes the arc over without a clear frame.
- Band: pixels whose centre lies between `VOL_ARC_R_IN_PX` (222) and `VOL_ARC_R_OUT_PX` (230) from the centre, written into the rotated output buffer. The inner radius stays above the listening ring's outermost rewritten radius (221) and the outer one below the glass edge (233). If the outer edge is cut off at the glass, setting `VOL_ARC_R_OUT_PX` back to 225 is the only change needed.
- Shape: 270° sweep with the gap at gravity-bottom, starting at the lower left. A pixel is fill when its threshold (1 to 100, one per 2.7°) is at or below the volume, else track. Fill `#F4F3EE`, track `#8A877F`; at 0 % the first three thresholds show in `#D14343`. Hard edges only, no anti-aliasing.
- The arc follows the quadrant target, not the ease: when the quadrant changes it is cleared and drawn once at the new position, also during the linger.
- Invalidation: the arc is invalidated as one area per 10 thresholds (at most 10 for the whole arc, one for a 1 % step). LVGL keeps 32 areas per frame before repainting the screen, so a frame in which the arc invalidates more than one area skips the listening-ring update; the ring catches up in the next frame.
- `VOL_ARC_*` values are plain configuration; the table is built at start-up into PSRAM.

## Screen orientation

The QMI8658 IMU keeps the mascot and bubble upright relative to gravity at any angle. The screen is round, so every angle fits.

- Angle: `atan2` of gravity in the screen plane, from the accelerometer at 50 Hz with a low-pass filter. The whole UI (mascot, bubble, ring) rotates as one group around the screen centre. The volume knob reads touch points in the panel frame and never subtracts the UI angle, so clockwise on the glass is clockwise at every orientation. The brightness swipe is the one gesture that uses the UI angle: it reads travel along the UI's up direction at the quadrant target latched when the touch starts. A LONG_PRESS must start within 150 px of the centre; a touch starting beyond that is an edge touch (see `protocol.md`, Input).
- All scene content stays within 225 px of the centre, so no angle pushes it off the screen. It also stays at least 2 px inside the volume arc's inner radius (`VOL_ARC_R_IN_PX`, 222 px); a host test checks both bounds against the constants. The arc itself (radius 222 to 230 px) is drawn in panel coordinates.
- Ignore changes smaller than 3°, and ease towards the new angle with a 150 ms time constant, so a hand-held board does not jitter.
- Within 5° of 0°, 90°, 180° or 270°, snap to that angle: there the block edges line up with the panel's square pixel grid and stay perfectly sharp. At other angles each block edge gets a one-pixel anti-aliased border.
- Flat on a desk (in-plane gravity below 0.5 g): keep the last angle.
- Quadrant mode (`ORIENT_FREE_ANGLE` 0, the default): the angle changes in 90° steps (switch 15° past a quadrant boundary after 500 ms). A step turns the whole group to the new right angle by the shorter way with a cubic ease-out over `ROTATE_EASE_MS` (plain configuration) and ends exactly on 0°, 90°, 180° or 270°. A new target during a turn restarts from the angle shown. While the panel is off the angle is taken at once.
- Performance gate: at least 25 fps while rotated and no audio underruns. If LVGL's layer transform is too slow, draw the blocks as rotated polygons; if that is still too slow, fall back to 90° steps (switch 15° past a quadrant boundary after 500 ms).
- The IMU-to-screen axis mapping is a board fact: take it from the BSP or Waveshare's examples, log the raw axes and the chosen angle as LOG frames, and have a person confirm by turning the board.
- Mechanism class: mathematical model (gravity angle with low-pass filter, dead zone and right-angle snap). The numbers above are plain configuration constants.
