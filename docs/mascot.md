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
| IDLE | Slow breathing bob; blink every few seconds; occasional glance; now and then a short sideways scuttle (legs alternate) and back. Slow position drift over minutes, and dimming after a long idle period, to limit AMOLED burn-in. |
| LISTENING | Claws raised, eyes open taller; a thin ring around the mascot whose width and brightness follow the mic level. |
| THINKING | Legs walk in place; eyes look up; the status bubble shows a spinner cycling through the Claude Code thinking glyphs `· ✢ ✳ ✶ ✻ ✽`. |
| SPEAKING | Body bounce and claw movement driven by the playback level. |
| ERROR | Eyes become crosses for a moment. |
| OFFLINE | Dimmed, eyes closed (one-row lines), slow breathing. |

Motion stays pixel-true: parts move in whole cells (or whole screen-pixel steps that keep cell edges sharp); no blur, fades or anti-aliased scaling of the mascot.

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

## Screen orientation

The QMI8658 IMU keeps the mascot and bubble upright relative to gravity at any angle. The screen is round, so every angle fits.

- Angle: `atan2` of gravity in the screen plane, from the accelerometer at 50 Hz with a low-pass filter. The whole UI (mascot, bubble, ring) rotates as one group around the screen centre. Taps are accepted anywhere, so touch needs no remapping.
- All content stays within 225 px of the centre, so no angle pushes it off the screen.
- Ignore changes smaller than 3°, and ease towards the new angle with a 150 ms time constant, so a hand-held board does not jitter.
- Within 5° of 0°, 90°, 180° or 270°, snap to that angle: there the block edges line up with the panel's square pixel grid and stay perfectly sharp. At other angles each block edge gets a one-pixel anti-aliased border.
- Flat on a desk (in-plane gravity below 0.5 g): keep the last angle.
- Performance gate: at least 25 fps while rotated and no audio underruns. If LVGL's layer transform is too slow, draw the blocks as rotated polygons; if that is still too slow, fall back to 90° steps (switch 15° past a quadrant boundary after 500 ms).
- The IMU-to-screen axis mapping is a board fact: take it from the BSP or Waveshare's examples, log the raw axes and the chosen angle as LOG frames, and have a person confirm by turning the board.
- Mechanism class: mathematical model (gravity angle with low-pass filter, dead zone and right-angle snap). The numbers above are plain configuration constants.
