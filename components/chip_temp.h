#pragma once

#include <stdbool.h>

/* The ESP32-S3's own temperature sensor (see chip_temp.c). */

/* Install the sensor and start sampling it in the background. */
void chip_temp_start(void);

/* The chip's temperature in °C, smoothed over about a minute, without any
 * calibration; false until the first reading (or if the sensor failed). */
bool chip_temp_raw(float *celsius);
