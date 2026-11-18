#include "orient.h"
#include <math.h>
#include "app_config.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_fx, s_fy, s_held, s_target, s_shown;
static bool s_seeded, s_override, s_ever;
static uint32_t s_pending_ms;

static float wrap180(float a)
{
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0) {
        a += 360.0f;
    }
    return a - 180.0f;
}

static float wrap360(float a)
{
    a = fmodf(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}

static float snap(float a)
{
    float q = roundf(a / 90.0f) * 90.0f;
    return fabsf(a - q) <= ORIENT_SNAP_DEG ? q : a;
}

void orient_feed(float up_x, float up_y, uint32_t dt_ms)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_ever) {
        s_fx = up_x;
        s_fy = up_y;
    } else {
        float a = (float)dt_ms / (ORIENT_LPF_TAU_MS + (float)dt_ms);
        s_fx += a * (up_x - s_fx);
        s_fy += a * (up_y - s_fy);
    }
    if (hypotf(s_fx, s_fy) >= ORIENT_FLAT_G && !s_override) {
        float raw = wrap360(atan2f(s_fx, -s_fy) * 180.0f / (float)M_PI);
#if ORIENT_FREE_ANGLE
        if (!s_seeded || fabsf(wrap180(raw - s_held)) >= ORIENT_DEAD_ZONE_DEG) {
            s_held = raw;
            s_target = snap(raw);
            if (!s_ever) {
                s_shown = s_target;
            }
        }
#else
        float cand = wrap360(roundf(raw / 90.0f) * 90.0f);
        if (!s_seeded) {
            s_held = s_target = cand;
            s_pending_ms = 0;
            if (!s_ever) {
                s_shown = cand;
            }
        } else if (fabsf(wrap180(raw - s_held)) > 45.0f + ORIENT_STEP_HYST_DEG && cand != s_held) {
            s_pending_ms += dt_ms;
            if (s_pending_ms >= ORIENT_STEP_HOLD_MS) {
                s_held = s_target = cand;
                s_pending_ms = 0;
            }
        } else {
            s_pending_ms = 0;
        }
#endif
        s_seeded = s_ever = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

void orient_override(bool on, float deg)
{
    portENTER_CRITICAL(&s_mux);
    s_override = on;
    s_seeded = false;
    if (on) {
        s_target = s_shown = wrap360(deg);
    }
    portEXIT_CRITICAL(&s_mux);
}

float orient_step(uint32_t dt_ms)
{
    portENTER_CRITICAL(&s_mux);
#if !ORIENT_FREE_ANGLE
    if (!s_override) {
        s_shown = s_target;
    }
#endif
    float d = wrap180(s_target - s_shown);
    s_shown = fabsf(d) < 0.05f ? s_target : wrap360(s_shown + d * (1.0f - expf(-(float)dt_ms / ORIENT_EASE_TAU_MS)));
    float out = s_shown;
    portEXIT_CRITICAL(&s_mux);
    return out;
}

float orient_target(void)
{
    return s_target;
}
