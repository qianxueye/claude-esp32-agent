#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Screen-up direction in screen axes (x right, y down), in g; call at the IMU rate. */
void orient_feed(float up_x, float up_y, uint32_t dt_ms);
void orient_override(bool on, float deg);
/* Advances the eased angle and returns it in degrees [0, 360). */
float orient_step(uint32_t dt_ms);
float orient_target(void);
