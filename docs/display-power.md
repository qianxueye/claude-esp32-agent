# Display power and burn-in protection

The panel is AMOLED, so wear follows level × lit time. The device lowers the level when nothing happens, turns the panel off after a longer rest, and lights it again on activity. This is local to the device: no state and no protocol frame changes, and INPUT frames are sent as before.

Constant classes: **M** mathematical model, **C** plain configuration. Every value lives in `firmware/main/app_config.h`; none is a run-time judgement or a console setting. The simulated constant c is pinned there by a test.

## Stages

| Stage | Panel | Entered |
|---|---|---|
| ACTIVE | `DISP_ACTIVE_PERCENT` (100 %) | any activity; one step up |
| DIM | `DISP_DIM_PERCENT` (10 %) | `DISP_HOLD_MS` after the last activity; four equal-ratio steps down |
| OFF | display-off command | `DISP_DIM_TO_OFF_MS` after DIM begins |

- Levels are percent of register 0x51. The steps down are `100·(10/100)^(n/4)` rounded: 56, 32, 18, 10 %, one every `DISP_RAMP_STEP_MS`. Panel brightness is one scalar for every pixel, so the pixel-true rule in `mascot.md` is unaffected.
- OFF uses `esp_lcd_panel_disp_on_off`. It keeps frame memory, wakes with one command, and rendering continues so the first frame after waking is current. `disp_sleep` is not used: it resets the panel and costs about 1.4 s and a flash.
- Waking from OFF sets the level first and then switches the panel on.

| Constant | Class | Meaning |
|---|---|---|
| `DISP_ACTIVE_PERCENT`, `DISP_DIM_PERCENT` | C | the two levels |
| `DISP_HOLD_MS` | C | ACTIVE time after the last activity |
| `DISP_DIM_TO_OFF_MS` | C | DIM time before OFF |
| `DISP_ATTN_CAP_MS` | C | ACTIVE limit in one unchanged non-IDLE state |
| `DISP_SETDOWN_HOLD_MS` | C | ACTIVE time left after a desk set-down |
| `DISP_RAMP_STEPS`, `DISP_RAMP_STEP_MS` | C | stepped descent |
| `DISP_STORM_BINS` | C | resolution of the storm-budget window (see below) |
| `ORBIT_*` | C | lattice size, pitch, origin and dwell |

## Activity

Activity restarts the hold and returns the stage to ACTIVE at once. Sources:

- touch press (the tap is still sent as TAP, also when the panel was dark);
- BOOT button press;
- a change into or out of LISTENING, THINKING, SPEAKING or ERROR;
- boot.

Not activity: PING and other host frames, a SET_STATE that repeats the current state, any change with OFFLINE at either end (including the OFFLINE to IDLE resume), and playback outside SPEAKING. A sleeping host would otherwise keep the screen lit overnight.

Attention cap: LISTENING, THINKING, SPEAKING and ERROR hold ACTIVE, but after `DISP_ATTN_CAP_MS` in one unchanged state the stage drops to DIM and never reaches OFF while that state lasts. A touch gives ACTIVE for another hold.

The IDLE palette dim is gone; the panel level carries the stage. The palette keeps only the OFFLINE look (`MASCOT_DIM_PERCENT`).

## Orbit

The drawing moves one lattice step every `ORBIT_DWELL_MS` to spread edge wear. The lattice is `ORBIT_COLS` × `ORBIT_ROWS` points at `ORBIT_PITCH_PX`, walked as a closed cycle that visits every point once: along row 0 and back in snake rows over columns 1 to 5, then up column 0. With 6 × 8 that is 48 steps, 48 minutes, x from −12 to +8 px and y from −4 to +24 px. The top row is bounded by the THINKING bubble: above y = −4 its top edge would leave the 320 px scene and be clipped (`test_orbit_keeps_drawing_in_scene` repeats the extents of `draw_mascot` and `draw_bubble`).

The orbit spreads edges only. The interior of the body is lit as long as the stage is ACTIVE; the level and the time in ACTIVE are the lever.

## Counters

The `wakes touch= button= state= motion=` log line, printed with the stats line, gives cumulative counts of brightening stage changes by cause; LOG frames can be dropped, so counts are cumulative. A brightening with no activity behind it (OFF to DIM in an attention state) is attributed to the state; the storm budget limit is `motion_budget`.

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
- Activity: a pickup, and every sample while LIFTED. MOVING counts as activity only when `MOTION_SMALL_WAKE_ENABLED` is 1; it is 0 until θ_on, θ_off and N_on are fitted to recorded traces.
- Turning the board about the vertical axis leaves gravity unchanged; it wakes only through incidental tilt.

| Constant | Class | Origin |
|---|---|---|
| `IMU_CTRL5_VALUE` | M | datasheet bandwidth table; below 25 Hz, half the poll rate |
| `MOTION_WINDOW_SAMPLES` | M, placeholder until traces | at least two periods of board ringing, at most a quarter of the latency target |
| `MOTION_REF_SAMPLES` | M, placeholder until traces | twice the 99th-percentile still interval in hand |
| `MOTION_THETA_ON_G`, `MOTION_THETA_OFF_G` | M, placeholder until traces | `θ_on = max(θ_noise, √(A_amb·S_min))`, `θ_off = (A_amb³·S_min)^¼`; A_amb is the largest D of the reject traces and S_min the 5th-percentile sustained D of the accept traces |
| `MOTION_N_ON` | M, placeholder until traces | `W + ⌈T_knock/dt⌉ + 1` |
| `MOTION_THETA_PICKUP_G`, `MOTION_N_PICKUP` | M | geometric mean of the largest nudge tilt (5°) and the smallest lift tilt (20°); travel bound above |
| `MOTION_TAU_REF_MS` | M | between 10·N_on·dt and θ_off / (drift rate) |
| `MOTION_PA_TAIL_MS` | M | window 100 ms + sensor filter settling + amplifier settle |

Detector tests: with θ = k·σ_D and σ_D = σ_W·√(1 + W/W_ref), 24 h of white rest noise at k = 4.44 gives at most one event. A look is one non-overlapping 100 ms mean; two consecutive looks above θ have probability q² with q = P(χ²₃ ≥ k²), so k = 3 gives about 740 pairs a day if the anchor carried the noise of a W_ref mean. The module's anchor is an exponential average, whose noise is far lower, so the expected count is about 500; `firmware/test/motion_test.c` computes it in closed form and checks the module's own D against it. The module's debounce of 14 samples is stricter than two looks, so it fires less often than the pair count.

## Storm budget

Only small-motion wakes (MOVING as activity, `MOTION_SMALL_WAKE_ENABLED`) draw on a budget: `DISP_STORM_BUDGET_MS` of ACTIVE per `DISP_STORM_PERIOD_MS` (sliding window of 60 bins). When it is spent they reach DIM at most, and the `disp:` line gives `cause=motion_budget`. Class M: a false-wake storm at 100 % costs no more panel wear than the resting dim level did before, `(0.174 − 0.10) / (1 − 0.10) = 8.2 %` of the time.

Pickups, touch, button and state activity are not budgeted. A push or a knock cannot fake a pickup (the travel bound above), and a budget for pickups would leave a screen that stays dark when the board is lifted after a few looks in an hour.

## Quick dim after set-down

When a LIFTED spell ends in a REST that passes the desk test and the device is in IDLE, the pickup hold shrinks to `DISP_SETDOWN_HOLD_MS` (class C) after that REST instead of `DISP_HOLD_MS`. Desk test: in the second half of the settle window the 100 ms mean stays within θ_noise of its running mean; θ_noise is the floor from the noise estimator below (valid after 64 rest blocks, blocks during LIFTED are not used). A hand holding the board still spreads by several mg and keeps the normal hold, as does a REST outside IDLE. Touch, BOOT, a state change or a new pickup after the set-down restore the normal hold.

## Noise floor

The floor raises θ_on and θ_off only when `MOTION_SMALL_WAKE_ENABLED` is 1; the desk test above always uses it. The floor needs no rest/motion decision, so it cannot lock up.

- Per 1 s block with the amplifier off, take the ten 100 ms means m_k of the raw samples: `A_b = Σ(m_{k+1} − m_k)² / 54` over three axes (the Allan variance of the mean).
- `σ̂_W² = (lower quartile of the last 64 blocks) / c`. Blocks that overlap amplifier time are dropped. The estimate is valid after 64 blocks.
- `σ_D = σ̂_W·√(1 + W/W_ref)` and `θ_noise = k·σ_D`. θ_on and θ_off are both raised to θ_noise when it is higher, so the settle test also tolerates the noise.
- k = 4.44: a look is one 100 ms mean; with the debounce spanning about two independent looks, `q = P(χ²₃ ≥ k²) = 2.0e-4` per look and `q² × 25.9e6 looks` is one event per 30 days.
- c = 0.760 is the median over 4000 simulated windows of (lower quartile) / σ_W² for white noise; the test pins it to ±1 %.
- Accuracy: one window gives σ̂ within ±5 % in about 90 % of windows (relative spread about 3 %). Disturbed blocks bias σ̂ upward, never downward: the lower quartile of 64 sits at rank 16 among the clean blocks, so with 30 % of blocks disturbed σ̂ is about 5 % high and with 70 % about 30 % high. The effect is fewer wakes, not more.

| Constant | Class |
|---|---|
| `MOTION_NOISE_K` | M |
| `MOTION_NOISE_C` | M |
| `MOTION_NOISE_BLOCKS`, `MOTION_NOISE_MEANS`, `MOTION_NOISE_RANK` | M |
| `MOTION_SMALL_WAKE_ENABLED` | C |
