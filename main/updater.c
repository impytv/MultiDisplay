/* Automatic firmware updates (see docs/auto-update-plan.md).
 *
 * The manifest at g_cfg->ota_url names the latest release: project, board,
 * version, image URL, size and SHA-256. Before anything is written, the
 * manifest must be for this project and board and offer a version newer than
 * the running one (and, for an automatic install, not the one that was just
 * rolled back). While the image streams in, its own app description must
 * name the same version; at the end its size and SHA-256 must match, and
 * ota_writer has esp_ota_end() check its signature. Only then is it booted.
 *
 * When: 10 minutes after boot (a check only, for the setup page), then at
 * the time set on the setup page and every so many hours from there (03:30
 * every 24 hours unless changed; see update_schedule.h) - the only checks
 * that install by themselves, and only when ticked on the setup page. A
 * failed check is retried after an hour, then two, ... up to a day. "Check
 * now" and "Install now" on the setup page run straight away. */

#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "adsb_client.h"
#include "ais_client.h"
#include "app.h"
#include "entur_client.h"
#include "http_util.h"
#include "ota_writer.h"
#include "rain_client.h"
#include "update_schedule.h"
#include "updater.h"
#include "version_util.h"
#include "watchdog.h"
#include "wifi_provision.h"
#include "restart.h"

static const char *TAG = "updater";

#define UPD_FIRST_CHECK_S  (10 * 60)
#define UPD_RETRY_MIN_S    3600
#define UPD_RETRY_MAX_S    (24 * 3600)
/* A scheduled check is still made this long into its period (at most half
 * of it), e.g. after a restart; later, it waits for the next. */
#define UPD_WINDOW_MIN     90
#define UPD_MANIFEST_MAX   4096
#define UPD_CHUNK          4096
#define UPD_TIMEOUT_MS     15000
#define UPD_NOTES_MAX      512

typedef enum { REQ_NONE, REQ_CHECK, REQ_INSTALL } request_t;

/* What the manifest offers. */
typedef struct {
    char version[32];
    char released[16];
    char notes[UPD_NOTES_MAX];
    char url[APP_CONFIG_OTA_URL_MAX + 64];
    uint32_t size;
    uint8_t sha256[32];
} offer_t;

/* For the setup page, guarded by s_lock (the httpd task reads it). */
static SemaphoreHandle_t s_lock;
static bool s_busy;
static int s_progress = -1;  /* percent while installing, else -1 */
static time_t s_checked;     /* wall time of the last check, 0 = none (or no clock) */
static bool s_checked_any;
static EXT_RAM_BSS_ATTR char s_result[160];
static EXT_RAM_BSS_ATTR offer_t s_offer;      /* when s_have_offer: a version newer than the running one */
static bool s_have_offer;

static TaskHandle_t s_task;
static request_t s_request; /* guarded by s_lock */
static int64_t s_next_us;    /* esp_timer time of the next scheduled check */
static int s_failures;
/* The scheduled period (see update_schedule.h) whose check has been made,
 * and the schedule it was counted in; UPD_NONE = none yet. */
#define UPD_NONE INT64_MIN
static int64_t s_period_done = UPD_NONE;
static uint16_t s_period_at;
static uint8_t s_period_every_h;

/* --------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static void set_result(const char *fmt, const char *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_result, sizeof(s_result), fmt, arg);
    s_checked_any = true;
    const time_t now = time(NULL);
    s_checked = now > PLAUSIBLE_EPOCH_S ? now : 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", s_result);
}

/* The version in the slot that was rolled back to the running one, or "". */
static void rolled_back_version(char *out, size_t out_len)
{
    out[0] = '\0';
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad != NULL && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        snprintf(out, out_len, "%.*s", (int)sizeof(d.version), d.version);
    }
}

static void show_status(const char *text)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(g_status_label, text);
        esp_lv_adapter_unlock();
    }
}

/* --------------------------------------------------------------------------
 * The manifest
 * ------------------------------------------------------------------------ */

/* Fetch and check the manifest into *o: NULL, or why not. */
static const char *fetch_manifest(offer_t *o)
{
    const esp_app_desc_t *me = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), "MultiDisplay/%s", me->version);
    char *body = NULL;
    if (http_get_body(g_cfg->ota_url, ua, UPD_TIMEOUT_MS, UPD_MANIFEST_MAX, &body, TAG) != ESP_OK) {
        return "fikk ikke hentet manifestet";
    }
    json_use_psram();
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return "manifestet er ikke gyldig JSON";
    }
    const char *err = NULL;
    const char *project = cJSON_GetStringValue(cJSON_GetObjectItem(root, "project"));
    const char *board = cJSON_GetStringValue(cJSON_GetObjectItem(root, "board"));
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    const char *sha = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sha256"));
    const cJSON *size = cJSON_GetObjectItem(root, "size");
    int v[3];
    memset(o, 0, sizeof(*o));
    if (project == NULL || strcmp(project, me->project_name) != 0) {
        err = "manifestet gjelder et annet prosjekt";
    } else if (board == NULL || strcmp(board, CONFIG_MULTIDISPLAY_BOARD) != 0) {
        err = "manifestet gjelder et annet kort";
    } else if (version == NULL || strlen(version) >= sizeof(o->version) || !version_parse(version, v)) {
        err = "manifestet har ingen gyldig versjon";
    } else if (url == NULL || url[0] == '\0' || !cJSON_IsNumber(size) || size->valuedouble <= 0 ||
               !hex_to_bytes32(sha, o->sha256)) {
        err = "manifestet mangler filens adresse, st\xC3\xB8" "rrelse eller SHA-256";
    } else {
        snprintf(o->version, sizeof(o->version), "%s", version);
        snprintf(o->released, sizeof(o->released), "%s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(root, "released")) ?: "");
        snprintf(o->notes, sizeof(o->notes), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(root, "notes")) ?: "");
        url_resolve(g_cfg->ota_url, url, o->url, sizeof(o->url));
        o->size = (uint32_t)size->valuedouble;
    }
    cJSON_Delete(root);
    return err;
}

/* --------------------------------------------------------------------------
 * The download
 * ------------------------------------------------------------------------ */

/* Stream the offered image into the other slot and select it: NULL, or why
 * not (nothing selected then). */
static const char *download(const offer_t *o)
{
    uint8_t *buf = heap_caps_malloc(UPD_CHUNK, MALLOC_CAP_SPIRAM);
    ota_writer_t *w = heap_caps_malloc(sizeof(*w), MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t client = NULL;
    const char *err = NULL;
    if (buf == NULL || w == NULL) {
        err = "ikke nok minne";
        goto out;
    }
    if ((err = ota_writer_start(w, o->size)) != NULL) {
        goto out;
    }
    const esp_http_client_config_t config = { .url = o->url, .timeout_ms = UPD_TIMEOUT_MS };
    client = esp_http_client_init(&config);
    if (client == NULL || esp_http_client_open(client, 0) != ESP_OK) {
        err = "fikk ikke kontakt med oppdateringssiden";
        goto abort;
    }
    const int64_t len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, o->url);
        err = "oppdateringssiden har ikke filen";
        goto abort;
    }
    if (len > 0 && len != o->size) {
        err = "filens st\xC3\xB8" "rrelse stemmer ikke med manifestet";
        goto abort;
    }
    ESP_LOGI(TAG, "Downloading %s (%u bytes)", o->url, (unsigned)o->size);

    size_t done = 0;
    int shown = -1;
    bool version_checked = false;
    while (done < o->size) {
        const int n = esp_http_client_read(client, (char *)buf, UPD_CHUNK);
        if (n <= 0) {
            err = "nedlastingen ble avbrutt";
            goto abort;
        }
        if ((err = ota_writer_write(w, buf, (size_t)n)) != NULL) {
            goto out; /* the writer has given up already */
        }
        done += (size_t)n;
        /* The image must be the version the manifest says it is. */
        const esp_app_desc_t *app = ota_writer_app(w);
        if (!version_checked && app != NULL) {
            version_checked = true;
            if (strncmp(app->version, o->version, sizeof(app->version)) != 0) {
                err = "filen er ikke versjonen manifestet oppgir";
                goto abort;
            }
        }
        const int pct = (int)(done * 100 / o->size);
        if (pct / 5 != shown / 5) {
            shown = pct;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_progress = pct;
            xSemaphoreGive(s_lock);
            char text[64];
            snprintf(text, sizeof(text), "Oppdaterer programvare %s... %d %%", o->version, pct);
            show_status(text);
            wd_weather_beat();
        }
    }
    show_status("Kontrollerer ny programvare...");
    err = ota_writer_finish(w, o->sha256);
    goto out;

abort:
    ota_writer_abort(w);
out:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    heap_caps_free(buf);
    heap_caps_free(w);
    return err;
}

/* --------------------------------------------------------------------------
 * A check
 * ------------------------------------------------------------------------ */

/* Check the manifest; install what it offers if `install` (and, when
 * `automatic`, it isn't the version just rolled back). Returns true if it
 * wrote on the status label. Restarts instead of returning after an
 * install. */
static bool run_check(bool install, bool automatic)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_busy = true;
    xSemaphoreGive(s_lock);

    offer_t *o = heap_caps_malloc(sizeof(*o), MALLOC_CAP_SPIRAM);
    bool drew = false;
    const char *err = o ? fetch_manifest(o) : "ikke nok minne";
    const char *running = esp_app_get_description()->version;
    char bad[32];
    rolled_back_version(bad, sizeof(bad));

    if (err != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_have_offer = false;
        xSemaphoreGive(s_lock);
        set_result("feilet: %s", err);
    } else if (version_compare(o->version, running) <= 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_have_offer = false;
        xSemaphoreGive(s_lock);
        set_result("oppdatert%s", "");
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_offer = *o;
        s_have_offer = true;
        xSemaphoreGive(s_lock);
        if (automatic && strcmp(bad, o->version) == 0) {
            set_result("%s gikk tilbake til forrige programvare etter installering; venter p\xC3\xA5" " en nyere",
                       o->version);
        } else if (!install) {
            if (g_cfg->ota_auto && s_period_done != UPD_NONE) {
                const int m = upd_period_start_min(s_period_done + 1, g_cfg->ota_at, g_cfg->ota_every_h);
                char msg[sizeof(o->version) + 48];
                snprintf(msg, sizeof(msg), "%s er tilgjengelig og installeres kl. %02d:%02d", o->version, m / 60,
                         m % 60);
                set_result("%s", msg);
            } else {
                set_result(g_cfg->ota_auto ? "%s er tilgjengelig og installeres ved neste planlagte sjekk"
                                           : "%s er tilgjengelig",
                           o->version);
            }
        } else {
            ESP_LOGI(TAG, "Installing %s over %s", o->version, running);
            /* Free what the screens' idle connections hold. */
            adsb_client_close();
            ais_client_close();
            rain_client_close();
            entur_client_close();
            drew = true;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_progress = 0;
            xSemaphoreGive(s_lock);
            const char *why = download(o);
            if (why == NULL) {
                set_result("%s installert, starter p\xC3\xA5" " nytt", o->version);
                show_status("Programvare oppdatert.\nStarter p\xC3\xA5 nytt...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                restart_device();
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "installering av %s feilet: %s", o->version, why);
            set_result("%s", msg);
            err = why;
        }
    }
    heap_caps_free(o);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_busy = false;
    s_progress = -1;
    xSemaphoreGive(s_lock);

    /* A failure is retried after an hour, then two, ... up to a day. */
    const int64_t now = esp_timer_get_time();
    if (err != NULL) {
        int64_t wait_s = (int64_t)UPD_RETRY_MIN_S << (s_failures < 5 ? s_failures : 5);
        s_next_us = now + (wait_s < UPD_RETRY_MAX_S ? wait_s : UPD_RETRY_MAX_S) * 1000000LL;
        s_failures++;
    } else {
        s_next_us = now + (int64_t)g_cfg->ota_every_h * 3600 * 1000000LL; /* matters only without a clock */
        s_failures = 0;
    }
    return drew;
}

/* Where local time is in the schedule: the period, how far into it and
 * the seconds until the next; false without a clock. */
static bool schedule_now(int64_t *period, int *into_min, int64_t *next_s)
{
    const time_t now = time(NULL);
    if (now <= PLAUSIBLE_EPOCH_S) {
        return false;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    *period = upd_period(upd_local_min(&lt), g_cfg->ota_at, g_cfg->ota_every_h, into_min);
    *next_s = ((int64_t)g_cfg->ota_every_h * 60 - *into_min) * 60 - lt.tm_sec;
    return true;
}

bool updater_poll(uint32_t *wait_ms)
{
    /* Taken under the lock: the web server sets it under the lock too, and
     * one landing between a read and a clear would be lost. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const request_t req = s_request;
    s_request = REQ_NONE;
    xSemaphoreGive(s_lock);
    if (g_cfg->ota_url[0] == '\0') {
        if (req != REQ_NONE) {
            set_result("ingen oppdateringsadresse satt%s", "");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_busy = false;
            xSemaphoreGive(s_lock);
        }
        return false;
    }
    int64_t period = 0, next_s = 0;
    int into = 0;
    const bool clock = schedule_now(&period, &into, &next_s);
    int window = g_cfg->ota_every_h * 30;
    if (window > UPD_WINDOW_MIN) {
        window = UPD_WINDOW_MIN;
    }
    const bool in_window = clock && into < window;
    if (clock && (s_period_done == UPD_NONE || s_period_at != g_cfg->ota_at ||
                  s_period_every_h != g_cfg->ota_every_h)) {
        /* Just started, or the schedule changed: this period's check is
         * still to come only if it has just begun. */
        s_period_done = in_window ? period - 1 : period;
        s_period_at = g_cfg->ota_at;
        s_period_every_h = g_cfg->ota_every_h;
    }
    const bool auto_install = g_cfg->ota_auto && in_window;
    bool drew = false;

    if (req != REQ_NONE) {
        drew = run_check(req == REQ_INSTALL || auto_install, req != REQ_INSTALL);
    } else if (clock && period != s_period_done) {
        /* The scheduled check - the one that installs, when allowed. */
        s_period_done = period;
        drew = run_check(g_cfg->ota_auto, true);
    } else if (esp_timer_get_time() >= s_next_us) {
        drew = run_check(auto_install, true);
    }

    int64_t left_ms = (s_next_us - esp_timer_get_time()) / 1000;
    if (clock && next_s * 1000 < left_ms) {
        left_ms = next_s * 1000;
    }
    if (left_ms < (int64_t)*wait_ms) {
        *wait_ms = left_ms > 0 ? (uint32_t)left_ms : 0;
    }
    return drew;
}

/* --------------------------------------------------------------------------
 * The setup page's side
 * ------------------------------------------------------------------------ */

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(o, "running", esp_app_get_description()->version);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(o, "busy", s_busy);
    cJSON_AddNumberToObject(o, "progress", s_progress);
    char when[24] = "";
    if (s_checked_any) {
        if (s_checked != 0) {
            struct tm lt;
            localtime_r(&s_checked, &lt);
            strftime(when, sizeof(when), "%d.%m %H:%M", &lt);
        } else {
            snprintf(when, sizeof(when), "nettopp");
        }
    }
    cJSON_AddStringToObject(o, "checked", when);
    cJSON_AddStringToObject(o, "result", s_result);
    if (s_have_offer) {
        cJSON *a = cJSON_AddObjectToObject(o, "available");
        cJSON_AddStringToObject(a, "version", s_offer.version);
        cJSON_AddStringToObject(a, "released", s_offer.released);
        cJSON_AddStringToObject(a, "notes", s_offer.notes);
    } else {
        cJSON_AddNullToObject(o, "available");
    }
    xSemaphoreGive(s_lock);
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

static esp_err_t request(httpd_req_t *req, request_t what)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_busy;
    if (!busy) {
        s_busy = true; /* shows as busy until the weather task gets to it */
        s_request = what;
    }
    xSemaphoreGive(s_lock);
    if (!busy) {
        xTaskNotifyGive(s_task);
    }
    return httpd_resp_sendstr(req, busy ? "busy" : "ok");
}

static esp_err_t h_check(httpd_req_t *req)
{
    return request(req, REQ_CHECK);
}

static esp_err_t h_install(httpd_req_t *req)
{
    return request(req, REQ_INSTALL);
}

void updater_start(TaskHandle_t task)
{
    s_lock = xSemaphoreCreateMutex();
    s_task = task;
    s_next_us = esp_timer_get_time() + UPD_FIRST_CHECK_S * 1000000LL;
    json_use_psram();
    ESP_LOGI(TAG, "Running firmware %s; updates %s from %s", esp_app_get_description()->version,
             g_cfg->ota_auto ? "automatic" : "manual", g_cfg->ota_url[0] ? g_cfg->ota_url : "(no address)");
    wifi_provision_add_get_handler("/ota/status", h_status);
    wifi_provision_add_post_handler("/ota/check", h_check);
    wifi_provision_add_post_handler("/ota/install", h_install);
}
