
/* Software watchdog: restart if the LVGL task (everything on screen) or the
 * weather task (all the fetching) stops making progress. A deadlock or a
 * call that never returns would otherwise leave the screen frozen on old
 * data for good: the task watchdog only logs here (ESP_TASK_WDT_PANIC is
 * off), and only notices busy loops, not a blocked task. */

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app.h"
#include "watchdog.h"

static const char *TAG = "watchdog";

#define WD_LVGL_MAX_S       60
/* One pass of the weather task is at most a 5-minute wait plus its fetches
 * (15 s timeouts; the rain radar's hour of images is the longest run). */
#define WD_WEATHER_MAX_S    (20 * 60)
#define WD_TRIP_LVGL        0x57444c56u
#define WD_TRIP_WEATHER     0x57445754u

/* esp_timer_get_time() of each task's last sign of life; 0 = not watched
 * (the weather task sits in the setup portal indefinitely, legitimately). */
static volatile int64_t s_wd_lvgl_us;
static volatile int64_t s_wd_weather_us;
/* Survives the restart, so the next boot can say why it happened. */
static RTC_NOINIT_ATTR uint32_t s_wd_tripped;

void wd_weather_beat(void)
{
    s_wd_weather_us = esp_timer_get_time();
}

static void wd_lvgl_beat_cb(lv_timer_t *t)
{
    (void)t;
    s_wd_lvgl_us = esp_timer_get_time();
}

static void wd_check_cb(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const int64_t lvgl = s_wd_lvgl_us, weather = s_wd_weather_us;
    if (lvgl != 0 && now - lvgl > WD_LVGL_MAX_S * 1000000LL) {
        s_wd_tripped = WD_TRIP_LVGL;
    } else if (weather != 0 && now - weather > WD_WEATHER_MAX_S * 1000000LL) {
        s_wd_tripped = WD_TRIP_WEATHER;
    } else {
        return;
    }
    ESP_LOGE(TAG, "Watchdog: the %s task has stalled - restarting",
             s_wd_tripped == WD_TRIP_LVGL ? "LVGL" : "weather");
    esp_restart();
}

/* Start watching, and report whether the watchdog caused this boot. */
void wd_start(void)
{
    if (esp_reset_reason() == ESP_RST_SW &&
        (s_wd_tripped == WD_TRIP_LVGL || s_wd_tripped == WD_TRIP_WEATHER)) {
        ESP_LOGW(TAG, "Restarted by the watchdog: the %s task had stalled",
                 s_wd_tripped == WD_TRIP_LVGL ? "LVGL" : "weather");
    }
    s_wd_tripped = 0;
    s_wd_lvgl_us = esp_timer_get_time();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_timer_create(wd_lvgl_beat_cb, 1000, NULL);
        esp_lv_adapter_unlock();
    }
    const esp_timer_create_args_t args = { .callback = wd_check_cb, .name = "watchdog" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 10 * 1000000LL));
}
