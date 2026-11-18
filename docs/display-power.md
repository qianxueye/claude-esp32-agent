# Display power and burn-in protection

The panel is AMOLED, so wear follows level × lit time. The device lowers the level when nothing happens, turns the panel off after a longer rest, and lights it again on activity. This is local to the device: no state and no protocol frame changes, and INPUT frames are sent as before.

Constant classes: **M** mathematical model, **C** plain configuration. Every value lives in `firmware/main/app_config.h`; none is a run-time judgement or a console setting.

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
| `DISP_RAMP_STEPS`, `DISP_RAMP_STEP_MS` | C | stepped descent |

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

The drawing moves one lattice step every `ORBIT_DWELL_MS` to spread edge wear. The lattice is `ORBIT_COLS` × `ORBIT_ROWS` points at `ORBIT_PITCH_PX`, walked as a closed cycle that visits every point once: along row 0 and back in snake rows over columns 1 to 5, then up column 0. With 6 × 8 that is 48 steps, 48 minutes, x from −12 to +8 px and y from −16 to +12 px.

The orbit spreads edges only. The interior of the body is lit as long as the stage is ACTIVE; the level and the time in ACTIVE are the lever.

## Refresh while OFF

The panel's TE output may stop while it is off. `refr_cb` therefore does not wait for TE while the panel is off and counts no missed TE; rendering continues at the UI timer rate.

## Test aids

DEBUG keys in `protocol.md`: `disp_scale` runs the timeline N times faster (and restarts it), `disp_force` pins a stage, `orbit` pins a lattice index, `fill` shows a flat grey for ghost inspection. Host tests: `firmware/test/run.sh`.
