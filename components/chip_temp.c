/* The ESP32-S3's built-in temperature sensor. It measures the chip itself,
 * which runs well above the room (the CPU, WiFi and the display driver warm
 * it), so the clock screen adds an offset measured against a thermometer
 * (app_config_t.temp_offset_dc, set on the setup page). The chip's warmth
 * also follows how busy it is, so readings are averaged: one every
 * SAMPLE_MS, smoothed over about a minute. */

#include <math.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "chip_temp.h"

static const char *TAG = "chip_temp";

#define SAMPLE_MS 5000
#define SMOOTH    0.1f /* weight of each new sample: about a minute */

static temperature_sensor_handle_t s_sensor;
static volatile float s_avg = NAN;

static void sample_cb(void *arg)
{
    (void)arg;
    float c;
    if (temperature_sensor_get_celsius(s_sensor, &c) != ESP_OK) {
        return;
    }
    s_avg = isnan(s_avg) ? c : s_avg + SMOOTH * (c - s_avg);
}

void chip_temp_start(void)
{
    if (s_sensor != NULL) {
        return;
    }
    /* The chip runs at 30-60 °C; -10..80 is the range with the least error
     * (under 1 °C, per the datasheet). */
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_sensor);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor: %s", esp_err_to_name(err));
        return;
    }
    sample_cb(NULL);
    const esp_timer_create_args_t args = { .callback = sample_cb, .name = "chip_temp" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, SAMPLE_MS * 1000ULL);
    }
}

bool chip_temp_raw(float *celsius)
{
    const float v = s_avg;
    if (isnan(v)) {
        return false;
    }
    *celsius = v;
    return true;
}
