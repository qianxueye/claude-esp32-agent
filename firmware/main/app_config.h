#pragma once

/* Values are plain configuration unless a comment says otherwise: "mathematical model" (docs/display-power.md) or fixed by docs/protocol.md. */

#define FW_VERSION "0.1.0"

/* Wire protocol (docs/protocol.md) */
#define PROTO_VERSION 3
#define PROTO_MAX_PAYLOAD 4096
#define PROTO_PARTIAL_FRAME_TIMEOUT_MS 200
#define PROTO_HOST_SILENCE_TIMEOUT_MS 5000
#define PROTO_SPK_STATUS_PERIOD_MS 100
#define STATS_LOG_PERIOD_MS 2000
#define LIFE_PERIOD_MS 100
#define HELLO_JSON_BYTES 192 /* plain configuration: the longest HELLO plus room for later fields; send_hello clamps */
#define HOST_JSON_BYTES 128
#define SNAPSHOT_QUEUE_DEPTH 8

/* Network link (docs/protocol.md "Links", "Pairing") */
#define NET_PAIR_MAX_BYTES 1023 /* PAIR text; its buffer keeps one more byte for the terminator */
#define NET_REMOTE_MAX_BYTES 511 /* SET_REMOTE payload; its buffer keeps one more byte */
#define NET_URL_MAX 255
#define WIFI_NETWORKS_MAX 4
#define NET_RELAY_NAMES_MAX 4
#define NET_RELAY_NAME_MAX 63
#define NET_RELAY_ADDRS_MAX 8
#define LAN_CONNECT_TIMEOUT_MS 1500 /* TCP connect to one relay address on the LAN */
#define REMOTE_CONNECT_TIMEOUT_MS 15000 /* DNS, TCP, outer TLS and upgrade of the remote URL */
#define NET_BACKOFF_MIN_MS 1000 /* relay and association retry, doubling */
#define NET_BACKOFF_MAX_MS 30000
#define NET_NVS_PARTITION "clawdnet" /* its own partition, so FORGET can erase its flash */
#define NET_NVS_NAMESPACE "clawdnet"
#define NET_INNER_HANDSHAKE_MS 10000 /* the relay's own handshake limit */
#define REMOTE_STEP_TIMEOUT_MS 5000 /* per blocking step of the tunnel connect; docs/network.md "Bounds" */
#define NET_SCAN_TIMEOUT_MS 10000
#define NET_ASSOC_TIMEOUT_MS 20000 /* association and DHCP */
#define NET_POLL_MS 20 /* socket wait per network task loop */
#define NET_WS_READ_TIMEOUT_MS 5000 /* rest of a WebSocket message once its first byte is readable */
#define NET_WS_WRITE_TIMEOUT_MS 5000
#define NET_WS_TX_BYTES 4352 /* one inner TLS record per WebSocket message */
#define NET_TX_RING_BYTES 32768 /* network frame queue; at least twice the largest frame */
#define NET_TX_LOCK_WAIT_MS 5
#define NET_READ_CHUNK_BYTES 1024
#define NET_EVENT_QUEUE_DEPTH 16
#define NET_STATUS_JSON_BYTES 128

/* USB-Serial-JTAG transport */
#define USB_RX_BUFFER_BYTES 16384
#define USB_TX_BUFFER_BYTES 32768
#define USB_RX_READ_CHUNK_BYTES 512
#define USB_RX_POLL_MS 10
#define TX_QUEUE_DEPTH 48
#define TX_SNAPSHOT_WRITE_TIMEOUT_MS 1000
#define LOG_LINE_MAX_BYTES 200

/* Audio */
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS 16
#define AUDIO_CHANNELS 2 /* codec I2S slots; payloads are mono */
#define AUDIO_FRAME_MS 20
#define AUDIO_FRAME_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)
#define AUDIO_FRAME_BYTES (AUDIO_FRAME_SAMPLES * 2)
#define SPK_QUEUE_BYTES 262144
#define DEFAULT_VOLUME 70
#define PA_IDLE_OFF_MS 300
#define PA_SETTLE_MS 20
#define MIC_GAIN_DB 30.0f
#define MIC_CH_LEFT 0
#define MIC_CH_RIGHT 1
#define MIC_CH_MIX 2
#define MIC_CHANNEL_MODE MIC_CH_LEFT
#define MIC_DISCARD_MS 200
#define SPK_UNDERRUN_GRACE_MS 40
#define SPK_SLOW_WRITE_MARGIN_MS 20
#define MIC_GAP_MARGIN_MS 20
#define MIC_LEVEL_FULL_SCALE 8000
#define SPK_LEVEL_FULL_SCALE 12000

/* Volume knob (docs/protocol.md "Volume", docs/mascot.md "Volume arc"); values marked placeholder are fitted from the logged knob lines */
#define TOUCH_SLOP_PX 13 /* plain configuration, placeholder: movement beyond this is not a tap or a hold */
#define VOL_R_MIN_PX 150 /* plain configuration, placeholder: a touch starting at this radius or beyond is an edge touch */
#define VOL_R_TRACK_PX 100 /* plain configuration, placeholder: an armed knob tolerates drift down to this radius */
#define VOL_ARM_DEG 15 /* mathematical model, placeholder: travel along the edge before the volume responds */
#define VOL_SWEEP_DEG 270 /* plain configuration: travel for 0 to 100 percent, also the arc sweep */
#define VOL_CW_SIGN 1 /* plain configuration: +1 when clockwise on the glass is rising touch angle */
#define VOL_JUMP_MIN_DEG 10 /* mathematical model, placeholder: base of the per-sample angle limit */
#define VOL_OMEGA_MAX_DPS 720 /* mathematical model, placeholder: fastest plausible finger, two turns per second */
#define VOL_JUMP_DT_CAP_MS 50 /* mathematical model, placeholder: limit growth stops at this sample gap */
#ifndef VOL_ARC_R_IN_PX
#define VOL_ARC_R_IN_PX 222 /* plain configuration: above the listening ring's outermost rewritten radius */
#endif
#ifndef VOL_ARC_R_OUT_PX
#define VOL_ARC_R_OUT_PX 230 /* plain configuration: below the glass edge at 233 */
#endif
#define VOL_ARC_START_DEG 135 /* plain configuration: UI-frame angle of the 0 percent end, clockwise from 3 o'clock */
#define VOL_ARC_MUTE_THR 3 /* plain configuration: pixels shown red at 0 percent */
#define VOL_ARC_CHUNK_THR 10 /* plain configuration: percent covered by one invalidated area */
#define VOL_ARC_LINGER_MS 800 /* plain configuration: the arc stays this long after release */
#define ARC_OVER_CELLS 3 /* plain configuration: most cells either rim arc reaches past an end */
#define ARC_OVER_C_NUM 1 /* mathematical model, placeholder: with ARC_OVER_C_DEN, stretch gain at the end relative to the in-range rate */
#define ARC_OVER_C_DEN 1 /* mathematical model, placeholder: see ARC_OVER_C_NUM */
#define ARC_SPRING_STEP_MS 60 /* plain configuration: time per cell while the stretch springs back */
#define ARC_OV_LOG_MAX 999999 /* plain configuration: log cap on the ov counters */
#define VOL_SAVE_MIN_MS 5000 /* mathematical model: flash wear, docs/protocol.md "Volume" */
#define VOL_NVS_NAMESPACE "clawd"
#define VOL_NVS_KEY "vol"

/* Brightness swipe (docs/display-power.md "User brightness"); values marked placeholder are fitted from the logged swipe lines */
#ifndef BRT_MIN_PERCENT
#define BRT_MIN_PERCENT 20 /* plain configuration: lowest user level; dpower.c asserts at least twice DISP_DIM_PERCENT */
#endif
#define BRT_STEPS 32 /* mathematical model: equal-ratio steps from BRT_MIN_PERCENT to 100, the finest that integer percent keeps distinct */
#define BRT_TRAVEL_PX 240 /* plain configuration, placeholder: travel along the UI's up direction for the whole range */
#define BRT_ARM_PX 48 /* mathematical model, placeholder: travel along the UI's up direction before the level responds */
#define BRT_LAT_PX 24 /* mathematical model, placeholder: sideways travel below which a swipe is never dropped as sideways */
#define BRT_CONE_NUM 1 /* mathematical model, placeholder: with BRT_CONE_DEN, the sideways-to-up ratio beyond which a swipe drops */
#define BRT_CONE_DEN 2 /* mathematical model, placeholder: 1/2 is a cone of 26.6 degrees either side of the UI vertical */
#define BRT_ARM_MAX_MS 500 /* mathematical model, placeholder: time from leaving the slop to arming */
#define BRT_JUMP_MIN_PX 32 /* mathematical model, placeholder: base of the per-sample distance limit */
#define BRT_V_MAX_PX_S 4800 /* mathematical model, placeholder: fastest plausible finger, the fastest logged stroke times its peak-to-mean ratio */
#define BRT_UP_SIGN 1 /* plain configuration: +1 when travel toward the top of the drawn UI raises the level */
#define BRT_BOUNCE_STEPS 0 /* plain configuration: ladder steps the panel dips when a swipe pushes past an end; 0 turns it off */
#define BRT_ARC_START_DEG 135 /* plain configuration: UI-frame angle of the brightness arc's lowest cell, clockwise from 3 o'clock */
#define BRT_ARC_SWEEP_DEG 90 /* plain configuration: the brightness arc spans the left rim */
#define BRT_ARC_CHUNK_CELLS 9 /* plain configuration: ladder cells covered by one invalidated area */
#define BRT_ARC_FILL_COLOR BUBBLE_FILL_COLOR /* plain configuration */
#define BRT_ARC_TRACK_COLOR BUBBLE_OFFLINE_FILL_COLOR /* plain configuration */
#define BRT_NVS_KEY "brt" /* plain configuration: NVS key in VOL_NVS_NAMESPACE */

/* Animation pack; format limits are in apack.h */
#define ANIM_PARTITION_LABEL "animpack" /* plain configuration: data partition of firmware/partitions.csv that holds the pack */
#define ANIM_DIM_PLAYS 0 /* plain configuration: 0 holds the frame while dim, 1 keeps playing */
#define ANIM_RING 1 /* plain configuration: 1 keeps the listening ring in pack mode */
#define ANIM_HEAD_MS 250 /* mathematical model: playback runway a clip head must cover while its slot starts, apsrc_test SR1 and SR9 */
#define ANIM_CARD_PIECE 4096 /* mathematical model: card read size, the largest that keeps the model's rate margin, apsrc_test SR9 */
#define ANIM_CARD_DEADLINE_MS 2000 /* plain configuration: how long the card may keep the pick waiting for flash */
#define ANIM_CARD_RETRY_S {60, 60, 300, 7200, 21600} /* plain configuration: waits before each card reopen after a read error */
#define ANIM_CARD_READ_RETRIES 2 /* plain configuration: in-line retries of a failed card read */
#define ANIM_CARD_READ_RETRY_MS 20 /* plain configuration: gap between in-line read retries */

/* Selftest */
#define SELFTEST_BEFORE_MS 400
#define SELFTEST_TONE_HZ 1000
#define SELFTEST_TONE_MS 1000
#define SELFTEST_TONE_AMPLITUDE 12000

/* Input */
#define TAP_MAX_MS 600
#define LONG_PRESS_MS 1500
#define BOOT_BUTTON_GPIO 0
#define BOOT_BUTTON_POLL_MS 10
#define BOOT_BUTTON_DEBOUNCE_POLLS 3
#define PWR_KEY_EXIO 4 /* plain configuration: TCA9554 pin of the PWR key, active high */
#define PWR_LOG_MIN_MS 200 /* plain configuration: minimum gap between TCA9554 input byte log lines */
#define PMU_I2C_ADDR 0x34 /* plain configuration: AXP2101, read-only */
#define PMU_I2C_HZ 400000 /* plain configuration */
#define PMU_I2C_TIMEOUT_MS 50 /* plain configuration */
#define BATT_POLL_MS 2000 /* plain configuration: PMU status poll period */
#define BATT_LOW_ENTER_PCT 20 /* plain configuration: the low look starts at or below this percentage */
#define BATT_LOW_EXIT_PCT 25 /* plain configuration: the low look ends at or above this percentage */
#define BATT_STALE_MS 10000 /* plain configuration: failed reads hide the icon after this long */
#define BATT_LOG_EVERY_MS 60000 /* plain configuration: the batt line repeats at this period */

/* Display geometry (docs/mascot.md); cell and bubble unit are whole screen pixels */
#define LCD_SIZE 466
#define MASCOT_CELL_PX 16
#define BUBBLE_UNIT_PX 4 /* font design pixel at 48 px; one quarter cell */
#define SCENE_SIZE_PX 480 /* square the group is drawn into before rotation; half of it is a multiple of MASCOT_CELL_PX and it covers the panel */
_Static_assert(SCENE_SIZE_PX % 32 == 0 && (SCENE_SIZE_PX / 32) % 2 == 1, "mathematical model: data-cache sets for the 90 degree column walk"); /* tested by aglue_test S1 */
#define DIRTY_BAND_ROWS 40 /* rotated output is invalidated in horizontal bands of this height */
#define SCENE_OUT_PX 466 /* rotated output square; must cover SCENE_MAX_RADIUS_PX of content */
#define SCENE_RING_CLEAR_PX (LISTEN_RING_DIAMETER / 2 - LISTEN_RING_MAX_WIDTH_PX - 1) /* derived: inner radius of the ring's rewrite zone, ui.c draw_ring */
#define SCENE_MAX_RADIUS_PX 225 /* glass bound; content also stays inside VOL_ARC_R_IN_PX, checked by volarc_test A2 */
#define MASCOT_COLOR 0xD97757
#define MASCOT_DIM_PERCENT 30
#define BUBBLE_FILL_COLOR 0xF4F3EE
#define BUBBLE_INK_COLOR 0x141413
#define BUBBLE_ERROR_COLOR 0xD14343
#define BUBBLE_OFFLINE_FILL_COLOR 0x8A877F
#define BUBBLE_SHIFT_UNITS 10 /* mascot moves down this many units while a bubble is shown */
#define BUBBLE_GAP_UNITS 3
#define BUBBLE_HEIGHT_UNITS 18 /* bubble body height */
#define BATT_GAP_UNITS 6 /* plain configuration: space between the legs and the battery outline */
#define BATT_CENTER_DX_PX -2 /* plain configuration: icon centre relative to the scene centre */
#define BATT_BESIDE_GAP_UNITS 2 /* plain configuration: space between a bubble and the icon beside it */
#define BATT_PLATE_UNITS 1 /* plain configuration: margin of the ink plate behind the icon over sticker art */
#define BATT_BESIDE_MAX_DX_PX 160 /* plain configuration: farthest right edge of the icon beside the top bubble, from the scene centre */
#define BUBBLE_PAD_UNITS 2
#define BUBBLE_POP_STEP_MS 60
#define BUBBLE_DOT_PULSE_MS 300
#define SPINNER_STEP_MS 150
#define LISTEN_RING_DIAMETER 440
#define LISTEN_RING_MIN_WIDTH_PX 2
#define LISTEN_RING_MAX_WIDTH_PX 8
#define LISTEN_RING_MIN_OPA 60
#define LISTEN_RING_STEPS 8 /* distinct ring states between quiet and loud */
#define DISPLAY_BUFFER_ROWS 32 /* rows per LVGL partial buffer, two buffers */
#define DISPLAY_BUFFER_IN_PSRAM 0
#define DISPLAY_TE_GPIO 13
#define DISPLAY_TE_WAIT_MS 20
#define INVAL_LOG_MAX 64
#define UI_LEVEL_TIMER_MS 16
#define UI_FPS_LOG_PERIOD_MS 2000

/* Mascot motion */
#define IDLE_BOB_PERIOD_MS 2400
#define IDLE_BLINK_PERIOD_MS 4000
#define IDLE_BLINK_MS 140
#define IDLE_GLANCE_PERIOD_MS 9000
#define IDLE_GLANCE_MS 900
#define IDLE_SCUTTLE_PERIOD_MS 13000
#define SCUTTLE_STEP_MS 120
#define SCUTTLE_HOLD_MS 700
#define SCUTTLE_DISTANCE_UNITS 4
#define THINK_LEG_STEP_MS 180
#define ERROR_CROSS_MS 1500
#define ERROR_SHAKE_MS 480
#define ERROR_SHAKE_STEP_MS 80
#define SPEAK_BOUNCE_MAX_UNITS 2
#define SPEAK_CLAW_STEP_MS 140
#define SPEAK_CLAW_LEVEL_MIN 120

/* Display power (docs/display-power.md) */
#define DISP_ACTIVE_PERCENT 100 /* plain configuration: the ACTIVE level until the user sets one; also the maximum */
#define DISP_DIM_PERCENT 10 /* plain configuration */
#define DISP_HOLD_MS 20000 /* plain configuration: ACTIVE time after the last activity */
#define DISP_DIM_TO_OFF_MS 1800000 /* plain configuration: DIM time before the panel turns off */
#define DISP_SETDOWN_HOLD_MS 1000 /* plain configuration: ACTIVE time left after a rest ends a pickup; the rest itself needs 2 s of stillness */
#define DISP_ATTN_CAP_MS 120000 /* plain configuration: ACTIVE limit in one unchanged non-IDLE state */
#define DISP_FLOOR_PERCENT 1 /* plain configuration: panel minimum; the fade before display-off ends here and a wake starts here */
#define DISP_RAMP_MS 300 /* plain configuration: equal-ratio brightness ramp between the two levels, both directions */
#define ORBIT_DWELL_MS 60000 /* plain configuration */
#define ORBIT_PITCH_PX 4 /* plain configuration: lattice pitch, one font design pixel */
#define ORBIT_COLS 6 /* plain configuration */
#define ORBIT_ROWS 8 /* plain configuration */
#define ORBIT_X0_PX -12 /* plain configuration: leftmost lattice column */
#define ORBIT_Y0_PX -4 /* plain configuration: topmost lattice row */
#define ORBIT_HALF_X_PX ((ORBIT_COLS - 1) * ORBIT_PITCH_PX / 2) /* derived: half range of the lattice in x */
#define ORBIT_HALF_Y_PX ((ORBIT_ROWS - 1) * ORBIT_PITCH_PX / 2) /* derived: half range in y */
#define ORBIT_RECENTRE_X_PX (-(ORBIT_X0_PX + ORBIT_HALF_X_PX)) /* derived: moves the lattice mid-point to the scene centre; sticker art only */
#define ORBIT_RECENTRE_Y_PX (-(ORBIT_Y0_PX + ORBIT_HALF_Y_PX)) /* derived, as above */
_Static_assert((ORBIT_COLS - 1) * ORBIT_PITCH_PX % 2 == 0 && (ORBIT_ROWS - 1) * ORBIT_PITCH_PX % 2 == 0, "the lattice mid-point is a whole pixel");

/* IMU orientation (docs/mascot.md "Screen orientation") */
#define IMU_I2C_ADDR_PRIMARY 0x6B
#define IMU_I2C_ADDR_SECONDARY 0x6A
#define IMU_WHO_AM_I 0x05
#define IMU_I2C_HZ 400000
#define IMU_I2C_TIMEOUT_MS 50
#define IMU_PERIOD_MS 20 /* 50 Hz */
#define IMU_RANGE_G 8.0f
#define IMU_CTRL1_VALUE 0x40 /* register address auto-increment */
#define IMU_CTRL2_VALUE 0x26 /* +-8 g, 125 Hz */
#define IMU_DISABLE_SETTLE_MS 20 /* plain configuration: wait after disabling the sensor before writing its configuration */
#define IMU_STARTUP_SKIP_SAMPLES 10 /* plain configuration: samples dropped after enabling the sensor, while its filter settles */
#define IMU_CTRL5_VALUE 0x07 /* mathematical model: accelerometer low-pass on, mode 11 (16.7 Hz), below half the poll rate */
#define IMU_CTRL7_ACCEL_ON 0x01
#define IMU_UP_X_SRC 1 /* screen up-vector x = +accel Y, y = -accel X (Waveshare 04_Immersive_block) */
#define IMU_UP_X_SIGN 1
#define IMU_UP_Y_SRC 0
#define IMU_UP_Y_SIGN -1
#define IMU_LOG_PERIOD_MS 2000
#define ORIENT_LPF_TAU_MS 100.0f
#define ORIENT_DEAD_ZONE_DEG 3.0f
#define ORIENT_EASE_TAU_MS 150.0f
#define ORIENT_SNAP_DEG 5.0f
#define ORIENT_FLAT_G 0.5f
#define ORIENT_FREE_ANGLE 0 /* 1 = free angle with dead zone and snap; 0 = 90 degree steps */
#define ORIENT_STEP_HYST_DEG 15.0f
#define ORIENT_STEP_HOLD_MS 500
#define ROTATE_EASE_MS 300 /* plain configuration: ease-out time of a quadrant change */

/* Motion detector (docs/display-power.md); a g value is the length of the mean-acceleration deviation */
#define MOTION_WINDOW_SAMPLES 5 /* mathematical model, placeholder until traces: mean window, 100 ms */
#define MOTION_REF_SAMPLES 100 /* mathematical model, placeholder until traces: rest anchor and settle window, 2 s */
#define MOTION_THETA_ON_G 0.013f /* mathematical model, placeholder until traces */
#define MOTION_THETA_OFF_G 0.008f /* mathematical model, placeholder until traces */
#define MOTION_N_ON 14 /* mathematical model, placeholder until traces: samples above THETA_ON before MOVING */
#define MOTION_THETA_PICKUP_G 0.174f /* mathematical model: 10 degrees of tilt */
#define MOTION_N_PICKUP 25 /* mathematical model: samples above THETA_PICKUP before LIFTED */
#define MOTION_TAU_REF_MS 20000 /* mathematical model: rest anchor follows the mean with this time constant */
#define MOTION_PA_TAIL_MS 200 /* mathematical model: window + sensor filter settling + amplifier settle after the amplifier stops */

/* Task priorities and stacks */
#define PRIO_RX 7
#define PRIO_MIC 9
#define PRIO_SPK 9
#define PRIO_TX 6
#define PRIO_LIFE 4
#define PRIO_BUTTON 3
#define PRIO_IMU 3
#define PRIO_BLIT 5
#define PRIO_WORKER 5
#define PRIO_SKIN_VERIFY 3 /* plain configuration: below the UI, audio and rx tasks */
#define PRIO_NET 5
#define STACK_RX 6144 /* PAIR parsing and its NVS write run on it */
#define STACK_TX 4096
#define STACK_MIC 6144
#define STACK_SPK 6144
#define STACK_LIFE 6144
#define STACK_BUTTON 3072
#define STACK_IMU 4096
#define STACK_BLIT 3072
#define STACK_WORKER 6144
#define STACK_SKIN_VERIFY 3072
#define STACK_NET 12288 /* inner and outer mbedTLS calls nest on it */
