#pragma once

/* Starts the QMI8658 accelerometer task; a missing sensor leaves the angle at its last value. */
void imu_start(void);
