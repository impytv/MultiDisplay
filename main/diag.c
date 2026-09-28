/* Diagnostics over WiFi, so a fault can be looked into without the USB
 * cable (opening the serial port resets the board, losing what led up to
 * it):
 *
 *  - /log: the last LOG_RING bytes of the log, kept in PSRAM, each line with
 *    the wall-clock time once the clock is set;
 *  - /coredump: the crash dump in the "coredump" partition, if any, for
 *    `idf.py coredump-info -c <file>`; POST /coredump/erase removes it;
 *  - /status: firmware, uptime, why it last restarted, WiFi signal, memory,
 *    and each service's last success and failure, as JSON for the setup
 *    page. */

#include "esp_attr.h"
#include "sdkconfig.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app.h"
#include "clock.h"
#include "diag.h"
#include "watchdog.h"
#include "wifi_provision.h"

static const char *TAG = "diag";

#define LOG_RING         (16 * 1024)
#define LOG_LINE_MAX     384
#define OFFLINE_AFTER_US (2 * 60 * 1000000LL) /* every fetch failing this long = no internet */

/* --------------------------------------------------------------------------
 * The log ring
 * ------------------------------------------------------------------------ */

static char *s_ring;
static size_t s_ring_head;   /* where the next byte goes */
static bool s_ring_full;     /* has wrapped at least once */
static SemaphoreHandle_t s_ring_lock;
static vprintf_like_t s_prev_vprintf;
static char s_line[LOG_LINE_MAX + 12];

static void ring_put(const char *p, size_t n)
{
    while (n > 0) {
        size_t take = LOG_RING - s_ring_head;
        if (take > n) {
            take = n;
        }
        memcpy(s_ring + s_ring_head, p, take);
        s_ring_head += take;
        p += take;
        n -= take;
        if (s_ring_head == LOG_RING) {
            s_ring_head = 0;
            s_ring_full = true;
        }
    }
}

/* Every log line goes to the serial port as before, and into the ring. */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    const int ret = s_prev_vprintf(fmt, ap);
    /* Never wait: a line that comes while another is being stored is only
     * missing from /log. */
    if (!xPortInIsrContext() && xSemaphoreTake(s_ring_lock, 0) == pdTRUE) {
        size_t n = 0;
        const time_t now = time(NULL);
        if (now > PLAUSIBLE_EPOCH_S) {
            struct tm lt;
            localtime_r(&now, &lt);
            n = strftime(s_line, sizeof(s_line), "%H:%M:%S ", &lt);
        }
        const int len = vsnprintf(s_line + n, sizeof(s_line) - n, fmt, copy);
        if (len > 0) {
            if ((size_t)len < sizeof(s_line) - n) {
                n += (size_t)len;
            } else {
                n = sizeof(s_line) - 1; /* cut short: keep the line break */
                s_line[n - 1] = '\n';
            }
            ring_put(s_line, n);
        }
        xSemaphoreGive(s_ring_lock);
    }
    va_end(copy);
    return ret;
}

void diag_init(void)
{
    s_ring = heap_caps_malloc(LOG_RING, MALLOC_CAP_SPIRAM);
    s_ring_lock = xSemaphoreCreateMutex();
    if (s_ring != NULL && s_ring_lock != NULL) {
        s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    }
}

static esp_err_t h_log(httpd_req_t *req)
{
    if (s_ring == NULL) {
        return httpd_resp_send_404(req);
    }
    char *copy = heap_caps_malloc(LOG_RING, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return httpd_resp_send_500(req);
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t n;
    if (s_ring_full) {
        /* Oldest first, starting after the first (probably cut) line. */
        const size_t tail = LOG_RING - s_ring_head;
        memcpy(copy, s_ring + s_ring_head, tail);
        memcpy(copy + tail, s_ring, s_ring_head);
        n = LOG_RING;
    } else {
        memcpy(copy, s_ring, s_ring_head);
        n = s_ring_head;
    }
    xSemaphoreGive(s_ring_lock);
    size_t start = 0;
    if (s_ring_full) {
        const char *nl = memchr(copy, '\n', n);
        start = nl ? (size_t)(nl - copy) + 1 : 0;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, copy + start, n - start);
    heap_caps_free(copy);
    return err;
}

/* --------------------------------------------------------------------------
 * The crash dump
 * ------------------------------------------------------------------------ */

static esp_err_t h_coredump(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Ingen krasjdump lagret.\n");
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (part == NULL || buf == NULL) {
        heap_caps_free(buf);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < size && err == ESP_OK; off += 4096) {
        const size_t n = (size - off < 4096) ? size - off : 4096;
        err = esp_partition_read(part, addr - part->address + off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    heap_caps_free(buf);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t h_coredump_erase(httpd_req_t *req)
{
    esp_err_t err = esp_core_dump_image_erase();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, err == ESP_OK ? "Krasjdumpen er slettet.\n" : "Kunne ikke slette.\n");
}

/* --------------------------------------------------------------------------
 * Services and status
 * ------------------------------------------------------------------------ */

static const char *const SERVICE_NAMES[DIAG_SERVICE_COUNT] = {
    [DIAG_FORECAST] = "V\xC3\xA6rvarsel",
    [DIAG_NOWCAST] = "N\xC3\xA5varsel",
    [DIAG_ALERTS] = "Farevarsler",
    [DIAG_AIRCRAFT] = "Fly",
    [DIAG_SHIPS] = "Skip",
    [DIAG_RAIN] = "Nedb\xC3\xB8rsradar",
    [DIAG_DEPARTURES] = "Avganger",
    [DIAG_ROUTES] = "Flyruter",
    [DIAG_AIR] = "Luftkvalitet",
    [DIAG_POLLEN] = "Pollen",
};

typedef struct {
    int64_t ok_us, fail_us; /* esp_timer time of the last success / failure, 0 = none */
    time_t ok_at, fail_at;  /* the same in wall time, 0 if the clock wasn't set */
    char error[48];
} service_t;

static EXT_RAM_BSS_ATTR service_t s_svc[DIAG_SERVICE_COUNT];
static portMUX_TYPE s_svc_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_any_ok_us, s_any_fail_us;

static time_t wall_now(void)
{
    const time_t now = time(NULL);
    return now > PLAUSIBLE_EPOCH_S ? now : 0;
}

void diag_ok(diag_service_t svc)
{
    const int64_t now = esp_timer_get_time();
    const time_t wall = wall_now();
    portENTER_CRITICAL(&s_svc_mux);
    s_svc[svc].ok_us = s_any_ok_us = now;
    s_svc[svc].ok_at = wall;
    portEXIT_CRITICAL(&s_svc_mux);
}

void diag_fail(diag_service_t svc, esp_err_t err)
{
    const int64_t now = esp_timer_get_time();
    const time_t wall = wall_now();
    const char *name = esp_err_to_name(err);
    portENTER_CRITICAL(&s_svc_mux);
    s_svc[svc].fail_us = s_any_fail_us = now;
    s_svc[svc].fail_at = wall;
    snprintf(s_svc[svc].error, sizeof(s_svc[svc].error), "%s", name);
    portEXIT_CRITICAL(&s_svc_mux);
}

diag_net_t diag_net_state(void)
{
    if (!wifi_provision_is_up()) {
        return DIAG_NO_WIFI;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_svc_mux);
    const int64_t ok = s_any_ok_us, fail = s_any_fail_us;
    portEXIT_CRITICAL(&s_svc_mux);
    /* The last fetch failed, and none has worked for a while. */
    if (fail > ok && now - (ok ? ok : 0) > OFFLINE_AFTER_US) {
        return DIAG_NO_INTERNET;
    }
    return DIAG_ONLINE;
}

static const char *reset_reason_no(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "str\xC3\xB8m p\xC3\xA5";
    case ESP_RST_EXT:       return "reset-knappen";
    case ESP_RST_SW:        return "omstart fra programmet";
    case ESP_RST_PANIC:     return "krasj";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "vakthund i maskinvaren";
    case ESP_RST_BROWNOUT:  return "for lav spenning";
    case ESP_RST_USB:       return "USB";
    default:                return "ukjent";
    }
}

/* "28.09 17:01", or how long ago if the clock wasn't set then. */
static void when_text(char *out, size_t n, time_t at, int64_t us)
{
    if (us == 0) {
        snprintf(out, n, "aldri");
    } else if (at != 0) {
        struct tm lt;
        localtime_r(&at, &lt);
        strftime(out, n, "%d.%m %H:%M:%S", &lt);
    } else {
        snprintf(out, n, "for %lld s siden", (long long)((esp_timer_get_time() - us) / 1000000));
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(o, "versjon", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "oppetid_s", (double)(esp_timer_get_time() / 1000000));
    const char *trip = wd_boot_trip();
    char why[96];
    snprintf(why, sizeof(why), "%s%s%s", reset_reason_no(esp_reset_reason()), trip ? ": " : "",
             trip ? trip : "");
    cJSON_AddStringToObject(o, "omstart", why);
    char panic[96] = "";
    const bool dump = esp_core_dump_image_check() == ESP_OK;
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF /* only an ELF dump says why */
    if (dump && esp_core_dump_get_panic_reason(panic, sizeof(panic)) != ESP_OK) {
        panic[0] = '\0';
    }
#endif
    cJSON_AddBoolToObject(o, "krasjdump", dump);
    cJSON_AddStringToObject(o, "krasjgrunn", panic);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_dbm", ap.rssi);
    }
    cJSON_AddStringToObject(o, "ip", wifi_provision_get_ip());
    const char *src;
    const time_t set_at = clock_set_at(&src);
    char clock[48] = "ikke stilt";
    if (set_at != 0) {
        char t[24];
        when_text(t, sizeof(t), set_at, 1);
        snprintf(clock, sizeof(clock), "stilt %s via %s", t, src);
    } else if (clock_is_set()) {
        snprintf(clock, sizeof(clock), "stilt");
    }
    cJSON_AddStringToObject(o, "klokke", clock);
    cJSON *mem = cJSON_AddObjectToObject(o, "minne");
    cJSON_AddNumberToObject(mem, "intern_ledig", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "intern_lavest", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "intern_storste_blokk", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram_ledig", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(mem, "psram_lavest", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    cJSON *svcs = cJSON_AddArrayToObject(o, "tjenester");
    for (int i = 0; i < DIAG_SERVICE_COUNT; i++) {
        portENTER_CRITICAL(&s_svc_mux);
        const service_t s = s_svc[i];
        portEXIT_CRITICAL(&s_svc_mux);
        if (s.ok_us == 0 && s.fail_us == 0) {
            continue; /* not used since boot */
        }
        cJSON *e = cJSON_CreateObject();
        char t[32];
        cJSON_AddStringToObject(e, "navn", SERVICE_NAMES[i]);
        when_text(t, sizeof(t), s.ok_at, s.ok_us);
        cJSON_AddStringToObject(e, "sist_ok", t);
        cJSON_AddBoolToObject(e, "feiler", s.fail_us > s.ok_us);
        if (s.fail_us != 0) {
            when_text(t, sizeof(t), s.fail_at, s.fail_us);
            cJSON_AddStringToObject(e, "sist_feil", t);
            cJSON_AddStringToObject(e, "feil", s.error);
        }
        cJSON_AddItemToArray(svcs, e);
    }
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

void diag_start(void)
{
    if (esp_core_dump_image_check() == ESP_OK) {
        ESP_LOGW(TAG, "A crash dump is stored - fetch it from /coredump");
    }
    wifi_provision_add_get_handler("/log", h_log);
    wifi_provision_add_get_handler("/status", h_status);
    wifi_provision_add_get_handler("/coredump", h_coredump);
    wifi_provision_add_post_handler("/coredump/erase", h_coredump_erase);
}
