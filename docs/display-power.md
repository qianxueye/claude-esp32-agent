# Display power and burn-in protection

The panel is AMOLED, so wear follows level × lit time. The device lowers the level when nothing happens, turns the panel off after a longer rest, and lights it again on activity. This is local to the device: no state and no protocol frame changes, and INPUT frames are sent as before, except that a touch begun while the screen is dark sends none.

Constant classes: **M** mathematical model, **C** plain configuration. Every value lives in `firmware/main/app_config.h`; none is a run-time judgement or a console setting. The simulated constant c is pinned there by a test.

## Stages

| Stage | Panel | Entered |
|---|---|---|
| ACTIVE | the user brightness (see User brightness); `DISP_ACTIVE_PERCENT` (100 %) until one is set | any activity |
| DIM | `DISP_DIM_PERCENT` (10 %) | `DISP_HOLD_MS` after the last activity |
| OFF | fade to `DISP_FLOOR_PERCENT` (1 %), then display-off | `DISP_DIM_TO_OFF_MS` (30 min) after DIM begins |

- Levels are percent of register 0x51. The level moves between its values in equal-ratio steps of integer percent over `DISP_RAMP_MS`, in both directions, updated every UI tick: `from·(to/from)^(elapsed/DISP_RAMP_MS)`. A reversal continues from the current level. Panel brightness is one scalar for every pixel, so the pixel-true rule in `mascot.md` is unaffected.
- OFF uses `esp_lcd_panel_disp_on_off`. It keeps frame memory, wakes with one command, and rendering continues so the first frame after waking is current. `disp_sleep` is not used: it resets the panel and costs about 1.4 s and a flash.
- DIM to OFF: the level ramps from the dim level to `DISP_FLOOR_PERCENT` over `DISP_RAMP_MS` with the same ramp, and only then is display-off sent. The stage reads OFF from the start of the fade; the panel stays on until it ends. A wake during the fade reverses from the current level and never sends display-off.
- Waking from OFF sets the floor level, switches the panel on, then ramps up to the target; it does not start at the dim level.

| Constant | Class | Meaning |
|---|---|---|
| `DISP_ACTIVE_PERCENT` | C | ACTIVE level until the user sets one; also the maximum |
| `DISP_DIM_PERCENT` | C | DIM level, absolute |
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

A touch that begins while the stage is DIM or OFF only wakes the screen: it sends no TAP on release, no LONG_PRESS at 1500 ms and no volume or brightness change however far it moves. The decision is taken when the press starts, before the wake is posted (`dp_touch_press`); a tap after the wake is sent as usual. The BOOT and PWR keys are always sent.

Not activity: PING and other host frames, a SET_STATE that repeats the current state, any change with OFFLINE at either end (including the OFFLINE to IDLE resume), and playback outside SPEAKING. A sleeping host would otherwise keep the screen lit overnight.

Attention cap: LISTENING, THINKING, SPEAKING and ERROR hold ACTIVE, but after `DISP_ATTN_CAP_MS` in one unchanged state the stage drops to DIM and never reaches OFF while that state lasts. A touch gives ACTIVE for another hold.

The IDLE palette dim is gone; the panel level carries the stage. The palette keeps only the OFFLINE look (`MASCOT_DIM_PERCENT`).

## User brightness

The ACTIVE level is a user setting kept on the board. A vertical swipe on a lit screen that starts in the centre, or at the top or bottom rim and runs inward, changes it. While it is adjusted, an arc on the left rim shows the level (`mascot.md`, Brightness arc). The host never sees or sets it, and no protocol frame changes.

Gesture (`volknob.c`, the one touch classifier):

- Start, centre: a centre touch (start radius below `VOL_R_MIN_PX`) on a lit screen that moves more than `TOUCH_SLOP_PX` before LONG_PRESS was sent becomes a swipe candidate instead of being dropped. A touch that moves after LONG_PRESS is still dropped (`why=moved`). A dark press only wakes.
- Start, rim: an edge touch on a lit screen keeps its knob path unchanged and, once it has moved more than `TOUCH_SLOP_PX`, also carries a rim candidate. The candidate is tested on each changed sample before the knob, while the knob has not armed. It arms with the same tests as a centre candidate plus one: with `d` the displacement from the press point and `r̂0` the unit vector from the centre to the press point, the inward travel `−d·r̂0` must be at least the travel along the rim `|d × r̂0|`, otherwise it drops with `why=tangent`. A dropped rim candidate never ends the touch: the knob, its zone rule and the edge tap run as before. An armed rim swipe is the same armed swipe as a centre one and keeps `cls=edge`.
- Which rim starts arm: a stroke in the cone that starts on the top or bottom rim and runs inward arms brightness; a stroke along the rim, an outward stroke, and an up–down stroke that starts on the left or right rim (more than about 45° from the UI's vertical axis) stay with the knob. The swipe arms after 48 px of travel, before a knob turn from that start has swept 15°.
- Frame: "up" is the top of the UI as drawn, read once at press from the quadrant target (`orient_target`), not from the eased angle. With UI angle φ, UI-up in the touch frame is `(sin φ, −cos φ)`, taken from a table at the four quadrants; this is the inverse of the transform `rotblit` draws with. Travel `up` and sideways `lat` are measured from the press point in that frame. A UI rotation during the touch changes nothing: re-projecting would turn the same motion on the glass into sideways motion mid-gesture.
- Arming, tested in this order on every sample whose point changed:
  1. jump: the step from the previous changed sample exceeds `BRT_JUMP_MIN_PX` plus `BRT_V_MAX_PX_S` times the sample gap (gap capped at `VOL_JUMP_DT_CAP_MS`): dropped, `why=jump`;
  2. `|lat| > BRT_LAT_PX` and `|lat| > |up| · BRT_CONE_NUM / BRT_CONE_DEN`: dropped, `why=lateral`;
  3. more than `BRT_ARM_MAX_MS` since the sample that left the slop: dropped, `why=slow`;
  4. `|up| ≥ BRT_ARM_PX`: armed (a rim candidate first passes the inward test above).

  So a candidate arms only if it reaches 48 px of UI-up travel within 500 ms inside a cone of 26.6° either side of the UI vertical, with sideways travel up to 24 px always allowed. For a continuously sampled touch this is the box `|lat| ≤ 24, |up| < 48` left through the top or bottom; the cone form also arms a coarse sample that jumps past the box corner while staying in the cone. The centre zone is a start condition only.
- Armed: the arming travel is dead, so the level starts from the user level read when the swipe arms (a level set between press and arming is the start). Sideways travel, radius and time are no longer tested. A jump cancels the swipe and restores that level. Release or a lost touch commits; an armed swipe never sends TAP, and no swipe sends any INPUT.

Mapping (`brtmap.c`):

- Ladder: `L(i) = round(BRT_MIN_PERCENT · (100 / BRT_MIN_PERCENT)^(i / BRT_STEPS))`, i = 0…32: 20 21 22 23 24 26 27 28 30 31 33 35 37 38 40 43 45 47 49 52 55 58 60 64 67 70 74 78 82 86 90 95 100. Equal ratios are equal perceived steps to first order, and an equal-ratio ladder stays equal-ratio under any power-law response of register 0x51. 32 steps is the finest ladder whose levels stay distinct in integer percent at the minimum (each step at least 21/20); every unrounded level is at least 0.008 from a rounding boundary, so no maths library can flip one.
- Step rule: the knob's truncating accumulator, kept in pixels: travel along UI-up (times `BRT_UP_SIGN`) accumulates, and each `BRT_TRAVEL_PX / BRT_STEPS` (7.5 px) is one step. A reversal needs up to one extra step, so noise at a boundary does not flicker.
- Ends saturate and do not wrap. Outward travel at an end is counted as push and the accumulator is cleared, so turning back responds within one step. When the push reaches one step the brightness arc blinks once: it is cleared for `BRT_END_BLINK_MS` and then drawn again, two stepped states with no fade. It blinks again only after the level has left the end. The panel dip (`BRT_BOUNCE_STEPS` steps inward, ramping back over `DISP_RAMP_MS`) is off; setting `BRT_BOUNCE_STEPS` above 0 brings it back beside the blink.
- Every value, from storage, the swipe or a DEBUG key, is clamped to `BRT_MIN_PERCENT`..100 and snapped to the nearest ladder level.

Display power:

- `dp_set_active` is the one entry for the ACTIVE level and clamps it again. `BRT_MIN_PERCENT ≥ 2 · DISP_DIM_PERCENT` is a static assert in `dpower.c` (a compile with 19 must fail, checked by `run.sh`), so DIM is never brighter than ACTIVE.
- Following the finger is decided by level, not by the clock: settled at ACTIVE, the panel takes the new level in the next update; during a ramp toward ACTIVE (a wake, or a dip when it is on) only the end moves and the end time stays; in DIM or OFF only the target changes and the next wake ramps to it. A clock-based restart would lag the finger by 300 ms.
- Boot writes the stored level to the panel before the first frame. Restarting the timeline (`disp_scale`) keeps the level.
- DIM stays at 10 % absolute. Since the user level is at most 100 %, every burn-in figure computed for 100 % is an upper bound.

Storage: namespace `clawd`, key `brt`, one byte, through `nvstore.c`, which also holds the volume. The save rules are the volume's (`protocol.md`, Volume): only a swipe release that changed the level, or `brt_set`, marks a write; never during a swipe, never for a pin, never after a cancel; the write waits for the amplifier off and a state other than LISTENING, and 5 s after the previous write of either key (one clock for both). When both keys are due in the same slot, the key that was not attempted last writes (the volume when neither was); a failed attempt counts, so a key that keeps failing cannot hold the slot, and a due key waits at most one extra slot. Absent or out-of-range values give 100 % and write nothing; an off-ladder value loads as the nearest level and is rewritten once; a read error keeps 100 % in RAM for that boot and writes nothing. Boot line: `bright <n> loaded`, `bright 100 default, none stored` or `bright 100 default, storage off`.

Known limits: a straight vertical stroke that starts in the centre of a lit screen, or at the top or bottom rim and runs inward, such as a wipe or a thumb sliding in while picking the board up, arms. Its effect is bounded: the arming travel is dead, one step is 7.5 px and ×1.05, the floor is 20 %, nothing reaches the host, and one swipe undoes it. The touch controller reports one point and no contact size, so a palm cannot be told from a finger.

Logged data, so that the placeholders can be fitted:

- every 2 s, cumulative: `bright lvl= eff= panel= set= arms= rim= cancels= slides= stops= saves= in=`. `panel` is the percent last written to the panel, `rim` counts the arms that started at the rim, `slides` counts lit centre touches that became candidates, and `in` counts TAP and LONG_PRESS frames sent by touch (a swipe must not change it);
- for every touch that moved more than the slop: `swipe: end cls= q= why= arm= b0= b= dx= dy= up= pk= lat= arm_ms= r1= rev= gap= stp= ku= kl= dur=`. `q` is the latched quadrant, `why` the first reason recorded for the touch: the swipe candidate's drop reason, else the reason the touch was ended (`none` when the swipe armed and was released or lost, or when an edge touch armed the knob before its candidate dropped and was released; `jump` when such an armed swipe or knob was cancelled by a jump, so `why=jump arm=1` is a cancelled swipe), `b0` and `b` the start and end level in percent (−1 when the swipe never armed), `dx` and `dy` the raw displacement at the last sample, `up` the signed UI-up travel, `pk` its peak and `lat` the peak sideways travel, `r1` the last radius and `rev` the direction reversals while armed. `gap` is the longest time between changed samples in ms and `stp` the longest step in px, both counted from the press, so they are the inputs of the jump test. `ku` and `kl` are the signed `up` and `lat` at the sample where the candidate armed or dropped, 0 when it did neither. `rev` is capped at 999, `gap` and `dur` at 999999, so the line always fits the 199 bytes a LOG frame keeps. The `knob: end` line is unchanged and keeps the knob's reason, for example `why=zone` for a rim swipe.
- every 2 s, the `fps=` line ends with `stk=` before its bracket: the fewest bytes of the LVGL task's stack left free since boot. That task runs the touch handling and writes the `swipe: end` line.

| Constant | Class | Meaning |
|---|---|---|
| `BRT_MIN_PERCENT` | C | lowest user level, at least twice `DISP_DIM_PERCENT` |
| `BRT_STEPS` | M | ladder steps; the finest that integer percent keeps distinct |
| `BRT_TRAVEL_PX` | C, placeholder | UI-up travel for the whole range |
| `BRT_ARM_PX` | M, placeholder | dead travel before the level responds; about the knob's 15° at radius 190 |
| `BRT_LAT_PX` | M, placeholder | sideways travel always allowed before arming; half the arming travel |
| `BRT_CONE_NUM`, `BRT_CONE_DEN` | M, placeholder | beyond the floor, sideways travel may be at most 1/2 of the UI-up travel (26.6°) |
| rim inward test | M, placeholder | inward travel at least the travel along the rim (45°); fitted from `ku`, `kl` of `cls=edge` lines |
| `BRT_ARM_MAX_MS` | M, placeholder | arming deadline; at least 6.7 mm/s through the box |
| `BRT_JUMP_MIN_PX`, `BRT_V_MAX_PX_S` | M, placeholder | per-sample distance limit: 32 px plus 4800 px/s, the fastest logged stroke (2298 px/s mean) times the peak-to-mean ratio of a minimum-jerk stroke (1.875) plus 11 %; 109 px at 16 ms, 272 px at 50 ms or more. Fitted from `gap` and `stp` |
| `BRT_UP_SIGN` | C, board fact | +1 when travel toward the top of the UI raises the level |
| `BRT_BOUNCE_STEPS` | C | steps of the end-stop panel dip; 0, off |
| `BRT_END_BLINK_MS` | C | time the brightness arc is blank at an end |
| `BRT_ARC_START_DEG`, `BRT_ARC_SWEEP_DEG`, `BRT_ARC_CHUNK_CELLS`, `BRT_ARC_FILL_COLOR`, `BRT_ARC_TRACK_COLOR` | C | brightness arc (`mascot.md`) |
| `BRT_NVS_KEY` | C | storage key |

The touch frame is a board fact. A frame turned 180° reverses the swipe at every orientation and is fixed with `BRT_UP_SIGN`; a single mirrored axis reverses it at two orientations 90° apart and must be fixed in the touch flags (`display_start` in `ui.c`), not with a sign.

## Orbit

The drawing moves one lattice step every `ORBIT_DWELL_MS` to spread edge wear. The lattice is `ORBIT_COLS` × `ORBIT_ROWS` points at `ORBIT_PITCH_PX`, walked as a closed cycle that visits every point once: along row 0 and back in snake rows over columns 1 to 5, then up column 0. With 6 × 8 that is 48 steps, 48 minutes, x from −12 to +8 px and y from −4 to +24 px. The top row is bounded by the THINKING bubble: above y = −4 its top edge would leave the 320 px scene and be clipped (`test_orbit_keeps_drawing_in_scene` repeats the extents of `draw_mascot` and `draw_bubble`).

The orbit spreads edges only. The interior of the body is lit as long as the stage is ACTIVE; the level and the time in ACTIVE are the lever.

## Counters

The `wakes touch= button= state= motion= pwr_err=` log line, printed with the stats line, gives cumulative counts of brightening stage changes by cause; LOG frames can be dropped, so counts are cumulative. A brightening with no activity behind it (OFF to DIM in an attention state) is attributed to the state. `pwr_err` counts failed reads of the PWR key input (cumulative; a failed read counts as not pressed).

## Refresh while OFF

The panel's TE output may stop while it is off. `refr_cb` therefore does not wait for TE while the panel is off and counts no missed TE; rendering continues at the UI timer rate.

## Test aids

DEBUG keys in `protocol.md`: `disp_scale` runs the timeline N times faster (and restarts it), `disp_force` pins a stage, `orbit` pins a lattice index, `fill` shows a flat grey for ghost inspection, `brt` pins the ACTIVE brightness without storing it, `brt_set` sets and stores it, `brt_arc` shows the brightness arc at a ladder index. `devctl disp` sends them (`--brt N|null` for the pin, `--brt-arc N|null` for the arc); `devctl brightness [N]` stores a level and prints the next `bright` line, failing after 5 s without one. `devctl swipecheck SECONDS --label grip|wipe|free` holds ACTIVE and pings the board so it stays out of OFFLINE. It waits up to 5 s for a baseline `bright` line, runs the window from there, and then takes as final the first `bright` line that arrives at least `SWIPE_FINAL_GUARD_S` (0.5 s) after the window, waiting up to 5 s; swipe lines are collected until the final line, and the pin is released last. It reports the counter deltas, the unarmed slides by `why` with the largest `pk`, and each armed swipe. Verdict: no baseline or no final line fails; a counter lower in the final line than in the baseline (a board reset) fails; `grip` and `wipe` pass only when `arms` did not change, the level is unchanged and no swipe line has `arm=1`; `free`, where swipes are made on purpose, passes otherwise. The tool needs the `rim=` counter, so it finds no `bright` line on firmware without it. `devctl imu-dump SECONDS FILE` records raw samples for threshold calibration and rejects a trace with lost lines. Host tests: `firmware/test/run.sh`.

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
