# Display power and burn-in protection

The panel is AMOLED, so wear follows level × lit time. The device lowers the level when nothing happens, turns the panel off after a longer rest, and lights it again on activity. This is local to the device: no state and no protocol frame changes, and INPUT frames are sent as before, except that a touch begun while the screen is dark sends none.

Constant classes: **M** mathematical model, **C** plain configuration. Every value lives in `firmware/main/app_config.h`; none is a run-time judgement or a console setting. The simulated constant c is pinned there by a test.

## Stages

| Stage | Panel | Entered |
|---|---|---|
| ACTIVE | `DISP_ACTIVE_PERCENT` (100 %) | any activity |
| DIM | `DISP_DIM_PERCENT` (10 %) | `DISP_HOLD_MS` after the last activity |
| OFF | fade to `DISP_FLOOR_PERCENT` (1 %), then display-off | `DISP_DIM_TO_OFF_MS` (30 min) after DIM begins |

- Levels are percent of register 0x51. The level moves between its values in equal-ratio steps of integer percent over `DISP_RAMP_MS`, in both directions, updated every UI tick: `from·(to/from)^(elapsed/DISP_RAMP_MS)`. A reversal continues from the current level. Panel brightness is one scalar for every pixel, so the pixel-true rule in `mascot.md` is unaffected.
- OFF uses `esp_lcd_panel_disp_on_off`. It keeps frame memory, wakes with one command, and rendering continues so the first frame after waking is current. `disp_sleep` is not used: it resets the panel and costs about 1.4 s and a flash.
- DIM to OFF: the level ramps from the dim level to `DISP_FLOOR_PERCENT` over `DISP_RAMP_MS` with the same ramp, and only then is display-off sent. The stage reads OFF from the start of the fade; the panel stays on until it ends. A wake during the fade reverses from the current level and never sends display-off.
- Waking from OFF sets the floor level, switches the panel on, then ramps up to the target; it does not start at the dim level.

| Constant | Class | Meaning |
|---|---|---|
| `DISP_ACTIVE_PERCENT`, `DISP_DIM_PERCENT` | C | the two levels |
| `DISP_HOLD_MS` | C | ACTIVE time after the last activity |
| `DISP_DIM_TO_OFF_MS` | C | DIM time before OFF |
| `DISP_ATTN_CAP_MS` | C | ACTIVE limit in one unchanged non-IDLE state |
| `DISP_SETDOWN_HOLD_MS` | C | ACTIVE time left after a set-down REST |
| `DISP_FLOOR_PERCENT` | C | panel minimum: end of the fade before OFF and start of a wake from OFF |
| `DISP_RAMP_MS` | C | brightness ramp time |
| `ORBIT_*` | C | lattice size, pitch, origin and dwell |

## Activity

Activity restarts the hold and returns the stage to ACTIVE at once. Sources:

- touch press;
- BOOT or PWR key press;
- a change into or out of LISTENING, THINKING, SPEAKING or ERROR;
- boot.

A touch that begins while the stage is DIM or OFF only wakes the screen: it sends no TAP on release and no LONG_PRESS at 1500 ms. The decision is taken when the press starts, before the wake is posted (`dp_touch_press`); a tap after the wake is sent as usual. The BOOT and PWR keys are always sent.

Not activity: PING and other host frames, a SET_STATE that repeats the current state, any change with OFFLINE at either end (including the OFFLINE to IDLE resume), and playback outside SPEAKING. A sleeping host would otherwise keep the screen lit overnight.

Attention cap: LISTENING, THINKING, SPEAKING and ERROR hold ACTIVE, but after `DISP_ATTN_CAP_MS` in one unchanged state the stage drops to DIM and never reaches OFF while that state lasts. A touch gives ACTIVE for another hold.

The IDLE palette dim is gone; the panel level carries the stage. The palette keeps only the OFFLINE look (`MASCOT_DIM_PERCENT`).

## Orbit

The drawing moves one lattice step every `ORBIT_DWELL_MS` to spread edge wear. The lattice is `ORBIT_COLS` × `ORBIT_ROWS` points at `ORBIT_PITCH_PX`, walked as a closed cycle that visits every point once: along row 0 and back in snake rows over columns 1 to 5, then up column 0. With 6 × 8 that is 48 steps, 48 minutes, x from −12 to +8 px and y from −4 to +24 px. The top row is bounded by the THINKING bubble: above y = −4 its top edge would leave the 320 px scene and be clipped (`test_orbit_keeps_drawing_in_scene` repeats the extents of `draw_mascot` and `draw_bubble`).

The orbit spreads edges only. The interior of the body is lit as long as the stage is ACTIVE; the level and the time in ACTIVE are the lever.

## Counters

The `wakes touch= button= state= motion= pwr_err=` log line, printed with the stats line, gives cumulative counts of brightening stage changes by cause; LOG frames can be dropped, so counts are cumulative. A brightening with no activity behind it (OFF to DIM in an attention state) is attributed to the state. `pwr_err` counts failed reads of the PWR key input (cumulative; a failed read counts as not pressed).

## Refresh while OFF

The panel's TE output may stop while it is off. `refr_cb` therefore does not wait for TE while the panel is off and counts no missed TE; rendering continues at the UI timer rate.

## Test aids

DEBUG keys in `protocol.md`: `disp_scale` runs the timeline N times faster (and restarts it), `disp_force` pins a stage, `orbit` pins a lattice index, `fill` shows a flat grey for ghost inspection. `devctl disp` sends them. `devctl imu-dump SECONDS FILE` records raw samples for threshold calibration and rejects a trace with lost lines. Host tests: `firmware/test/run.sh`.

## Motion

The accelerometer (QMI8658) is polled every `IMU_PERIOD_MS` and all three axes feed `motion.c`. CTRL5 is written with `IMU_CTRL5_VALUE` (low-pass on, 16.7 Hz) and read back in the `imu: ctrl write` log line as `ctrl5=`.

- Signal: `D = |ḡ − g_ref|` in g. ḡ is the mean of the last `MOTION_WINDOW_SAMPLES` samples of the three axes. g_ref is the rest anchor: the mean of the window that last established rest, followed with time constant `MOTION_TAU_REF_MS` while D < θ_off to absorb offset drift.
- A knock, a keystroke or speaker vibration leaves the board in the same pose, so its acceleration averages to zero in the window and D returns to the anchor. A nudge or a lift changes the pose for hundreds of milliseconds.
- REST to MOVING: D ≥ θ_on for `MOTION_N_ON` consecutive samples. MOVING (or LIFTED) to REST: the mean stays within θ_off of its own running mean for `MOTION_REF_SAMPLES` samples; that mean becomes the anchor.
- Pickup: D ≥ θ_P for `MOTION_N_PICKUP` samples, from REST or MOVING, gives LIFTED until the next REST. A push that ends where it began cannot reach it: 0.174 g held for 0.5 s needs 0.85 m/s and 0.21 m of travel.
- Speaker mask: while the amplifier is on, and for `MOTION_PA_TAIL_MS` after it stops, REST to MOVING is not counted. Pickup stays armed, since zero-mean vibration cannot hold a 0.17 g offset.
- Activity: a pickup, and every sample while LIFTED. Only a pickup wakes the board: desk vibration, nudges and MOVING never do. MOVING and REST only bound a LIFTED spell.
- Turning the board about the vertical axis leaves gravity unchanged; it wakes only through incidental tilt.
- Remaining placeholders: θ_on, θ_off and N_on (they only decide when MOVING starts, which wakes nothing), the window and settle lengths, and the tilt limits behind θ_P (check θ_P and N_P against a recorded lift).

| Constant | Class | Origin |
|---|---|---|
| `IMU_CTRL5_VALUE` | M | datasheet bandwidth table; below 25 Hz, half the poll rate |
| `MOTION_WINDOW_SAMPLES` | M, placeholder until traces | at least two periods of board ringing, at most a quarter of the latency target |
| `MOTION_REF_SAMPLES` | M, placeholder until traces | twice the 99th-percentile still interval in hand |
| `MOTION_THETA_ON_G`, `MOTION_THETA_OFF_G` | M, placeholder until traces | θ_off ends a LIFTED spell (the settle test) and θ_on starts MOVING; both to be fitted to recorded traces |
| `MOTION_N_ON` | M, placeholder until traces | `W + ⌈T_knock/dt⌉ + 1` |
| `MOTION_THETA_PICKUP_G`, `MOTION_N_PICKUP` | M | geometric mean of the largest nudge tilt (5°) and the smallest lift tilt (20°); travel bound above |
| `MOTION_TAU_REF_MS` | M | between 10·N_on·dt and θ_off / (drift rate) |
| `MOTION_PA_TAIL_MS` | M | window 100 ms + sensor filter settling + amplifier settle |

Detector tests: with θ = k·σ_D and σ_D = σ_W·√(1 + W/W_ref), 24 h of white rest noise at k = 4.44 gives at most one event. A look is one non-overlapping 100 ms mean; two consecutive looks above θ have probability q² with q = P(χ²₃ ≥ k²), so k = 3 gives about 740 pairs a day if the anchor carried the noise of a W_ref mean. The module's anchor is an exponential average, whose noise is far lower, so the expected count is about 500; `firmware/test/motion_test.c` computes it in closed form and checks the module's own D against it. The module's debounce of 14 samples is stricter than two looks, so it fires less often than the pair count.

## Quick dim after set-down

Every REST that ends a LIFTED spell (event `MOTION_EV_SETDOWN`) while the device is in IDLE shrinks the pickup hold to `DISP_SETDOWN_HOLD_MS` (1000 ms, class C) after that REST instead of `DISP_HOLD_MS`. REST needs `MOTION_REF_SAMPLES` of stillness (2 s) first, so the screen dims about 3 s after the board reaches the desk. There is no test for desk versus hand: no dim while held has been observed, and a wrong call only dims to 10 %, which a tilt (new pickup) or a touch undoes. A REST outside IDLE, a REST that ends MOVING, and any later touch, BOOT, state change or new pickup keep or restore the normal hold.

Logged inputs, so that ordinary use produces data:

- `motion: ev=rest ... spread=<mg> from=<LIFTED|MOVING>`: the largest deviation of the 100 ms means from their running mean in the second half of the settle window, and the state the REST left;
- `disp: stage=DIM ... hold=<ms>`: the hold that applied (`DISP_SETDOWN_HOLD_MS` or `DISP_HOLD_MS`);
- `disp: panel=off|on level=<pct>`: the panel switch, with the level at that moment (1 at both ends).

A future in-hand classifier must be designed from logged set-down and in-hand spreads, not from assumed noise figures.
