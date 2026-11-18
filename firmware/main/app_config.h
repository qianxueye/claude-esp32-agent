#pragma once

/* Values are plain configuration unless a comment says otherwise: "mathematical model" (docs/display-power.md) or fixed by docs/protocol.md. */

#define FW_VERSION "0.1.0"

/* Wire protocol (docs/protocol.md) */
#define PROTO_VERSION 2
#define PROTO_MAX_PAYLOAD 4096
#define PROTO_PARTIAL_FRAME_TIMEOUT_MS 200
#define PROTO_HOST_SILENCE_TIMEOUT_MS 5000
#define PROTO_SPK_STATUS_PERIOD_MS 100
#define STATS_LOG_PERIOD_MS 2000
#define LIFE_PERIOD_MS 100
#define HELLO_JSON_BYTES 160
#define HOST_JSON_BYTES 128
#define SNAPSHOT_QUEUE_DEPTH 8

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

/* Display geometry (docs/mascot.md); cell and bubble unit are whole screen pixels */
#define LCD_SIZE 466
#define MASCOT_CELL_PX 16
#define BUBBLE_UNIT_PX 4 /* font design pixel at 48 px; one quarter cell */
#define SCENE_SIZE_PX 320 /* square the group is drawn into before rotation */
#define DIRTY_BAND_ROWS 40 /* rotated output is invalidated in horizontal bands of this height */
#define SCENE_OUT_PX 466 /* rotated output square; must cover SCENE_MAX_RADIUS_PX of content */
#define SCENE_MAX_RADIUS_PX 225
#define MASCOT_COLOR 0xD97757
#define MASCOT_DIM_PERCENT 30
#define BUBBLE_FILL_COLOR 0xF4F3EE
#define BUBBLE_INK_COLOR 0x141413
#define BUBBLE_ERROR_COLOR 0xD14343
#define BUBBLE_OFFLINE_FILL_COLOR 0x8A877F
#define BUBBLE_SHIFT_UNITS 10 /* mascot moves down this many units while a bubble is shown */
#define BUBBLE_GAP_UNITS 3
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
#define DISP_ACTIVE_PERCENT 100 /* plain configuration */
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
#define ORBIT_Y0_PX -4 /* plain configuration: topmost lattice row; the THINKING bubble reaches the scene top at this offset */

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
#define STACK_RX 4096
#define STACK_TX 4096
#define STACK_MIC 6144
#define STACK_SPK 6144
#define STACK_LIFE 4096
#define STACK_BUTTON 3072
#define STACK_IMU 4096
#define STACK_BLIT 3072
#define STACK_WORKER 6144
