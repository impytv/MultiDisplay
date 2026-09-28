/* Software watchdog: restart if the LVGL task (everything on screen) or the
 * weather task (all the fetching) stops making progress. A deadlock or a
 * call that never returns would otherwise leave the screen frozen on old
 * data for good: the task watchdog only logs here (ESP_TASK_WDT_PANIC is
 * off), and only notices busy loops, not a blocked task.
 *
 * It also watches the heap: a leak or fragmentation that leaves too little
 * memory for a minute restarts the device too, rather than leaving it to
 * fail in odd ways until the nightly restart - and the memory figures are
 * logged every hour, so a slow leak shows in the log long before that. */

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app.h"
#include "watchdog.h"

static const char *TAG = "watchdog";

#define WD_CHECK_S          10
#define WD_LVGL_MAX_S       60
/* One pass of the weather task is at most a 5-minute wait plus its fetches
 * (15 s timeouts; the rain radar's hour of images is the longest run). */
#define WD_WEATHER_MAX_S    (20 * 60)

/* Low memory, for WD_LOW_MEM_S on end: a fetch dips internal DRAM to about
 * 36 KB for a moment at worst, and WiFi's buffers start failing below a few
 * KB, or when no block of ~1.6 KB is left. PSRAM holds a few hundred KB of
 * cached screens beyond its steady ~1.3 MB free; a forecast parse needs
 * ~350 KB of it. */
#define WD_LOW_INTERNAL     (16 * 1024)
#define WD_LOW_INTERNAL_BLK (4 * 1024)
#define WD_LOW_PSRAM        (300 * 1024)
#define WD_LOW_MEM_S        60
#define WD_MEM_LOG_S        3600

#define WD_TRIP_LVGL        0x57444c56u
#define WD_TRIP_WEATHER     0x57445754u
#define WD_TRIP_MEMORY      0x5744454du

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

static const char *trip_reason(uint32_t trip)
{
    switch (trip) {
    case WD_TRIP_LVGL:    return "the LVGL task had stalled";
    case WD_TRIP_WEATHER: return "the weather task had stalled";
    case WD_TRIP_MEMORY:  return "memory had run low";
    default:              return NULL;
    }
}

static void log_memory(const char *what)
{
    ESP_LOGI(TAG, "%s: internal %u free (lowest %u, largest block %u), PSRAM %u free (lowest %u)", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* Whether memory is low right now. */
static bool memory_low(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < WD_LOW_INTERNAL ||
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < WD_LOW_INTERNAL_BLK ||
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < WD_LOW_PSRAM;
}

static void wd_check_cb(void *arg)
{
    (void)arg;
    static int low_checks, checks;
    const int64_t now = esp_timer_get_time();
    const int64_t lvgl = s_wd_lvgl_us, weather = s_wd_weather_us;

    if (++checks * WD_CHECK_S >= WD_MEM_LOG_S) {
        checks = 0;
        log_memory("Memory");
    }
    low_checks = memory_low() ? low_checks + 1 : 0;

    if (lvgl != 0 && now - lvgl > WD_LVGL_MAX_S * 1000000LL) {
        s_wd_tripped = WD_TRIP_LVGL;
    } else if (weather != 0 && now - weather > WD_WEATHER_MAX_S * 1000000LL) {
        s_wd_tripped = WD_TRIP_WEATHER;
    } else if (low_checks * WD_CHECK_S >= WD_LOW_MEM_S) {
        s_wd_tripped = WD_TRIP_MEMORY;
        log_memory("Low memory");
    } else {
        return;
    }
    ESP_LOGE(TAG, "Watchdog: %s - restarting", trip_reason(s_wd_tripped));
    esp_restart();
}

/* Start watching, and report whether the watchdog caused this boot. */
void wd_start(void)
{
    const char *why = trip_reason(s_wd_tripped);
    if (esp_reset_reason() == ESP_RST_SW && why != NULL) {
        ESP_LOGW(TAG, "Restarted by the watchdog: %s", why);
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
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, WD_CHECK_S * 1000000LL));
}
