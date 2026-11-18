#pragma once

/* Starts the QMI8658 accelerometer task; a missing sensor leaves the angle at its last value. */
void imu_start(void);
/* Logs the raw accelerometer counts of the next `seconds` seconds (0 stops), five samples per line with a running index. */
void imu_set_dump(int seconds);
