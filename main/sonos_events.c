/* The Sonos group's pushed changes (sonos_events.h). */

#include "sonos_events.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "wifi_provision.h"

static const char *TAG = "sonos_events";

#define EV_PATH      "/sonos/hendelse"
#define EV_TIMEOUT_S 600 /* asked for; renewed at half of what is granted */
#define EV_RETRY_US  (60 * 1000000LL) /* after a failed subscribe */

static const char *const SERVICE[] = { "AVTransport", "GroupRenderingControl" };
#define EV_SERVICES (sizeof(SERVICE) / sizeof(SERVICE[0]))

/* The weather task's, but the SIDs read by the web server's task too (under
 * s_lock). */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_sid[EV_SERVICES][SONOS_SID_MAX];
static char s_ip[16];        /* the speaker subscribed to */
static char s_uuid[32];      /* its room */
static char s_callback[64];  /* the address it sends to */
static int64_t s_renew_at;   /* esp_timer time to renew (or retry) */
static bool s_live;
static volatile int s_count;  /* events of ours since subscribing, for the log */

static esp_err_t h_notify(httpd_req_t *req)
{
    char sid[SONOS_SID_MAX] = "";
    httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid));
    bool ours = false;
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < EV_SERVICES && sid[0]; i++) {
        ours |= strcmp(sid, s_sid[i]) == 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    /* The body (the changes, as XML) isn't read: what plays is asked for
     * as it is shown. Read it off so the connection can be used again. */
    char buf[256];
    int left = req->content_len;
    while (left > 0) {
        const int n = httpd_req_recv(req, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n <= 0) {
            break;
        }
        left -= n;
    }
    /* 200 even when not ours: the first comes before the SID is noted, and
     * an old one runs out by itself. */
    if (ours) {
        if (s_count++ == 0) {
            ESP_LOGI(TAG, "First event in");
        }
        app_wake_poll();
    }
    return httpd_resp_send(req, NULL, 0);
}

void sonos_events_add_handler(void)
{
    if (wifi_provision_add_open_handler(EV_PATH, HTTP_NOTIFY, h_notify) != ESP_OK) {
        ESP_LOGW(TAG, "No handler for the speakers' events");
    }
}

static void set_sid(size_t i, const char *sid)
{
    taskENTER_CRITICAL(&s_lock);
    snprintf(s_sid[i], sizeof(s_sid[i]), "%s", sid);
    taskEXIT_CRITICAL(&s_lock);
}

void sonos_events_stop(void)
{
    for (size_t i = 0; i < EV_SERVICES; i++) {
        if (s_sid[i][0]) {
            char sid[SONOS_SID_MAX];
            memcpy(sid, s_sid[i], sizeof(sid));
            set_sid(i, "");
            sonos_unsubscribe(s_ip, SERVICE[i], sid); /* if it fails, it runs out */
        }
    }
    if (s_live) {
        ESP_LOGI(TAG, "Events from %s ended after %d", s_ip, s_count);
    }
    s_ip[0] = s_uuid[0] = s_callback[0] = '\0';
    s_renew_at = 0;
    s_live = false;
    s_count = 0;
}

void sonos_events_keep(const sonos_room_t *room)
{
    const char *me = wifi_provision_get_ip();
    if (me == NULL || me[0] == '\0') {
        return;
    }
    char callback[sizeof(s_callback)];
    snprintf(callback, sizeof(callback), "http://%s" EV_PATH, me);
    if (strcmp(room->uuid, s_uuid) != 0 || strcmp(room->ip, s_ip) != 0 || strcmp(callback, s_callback) != 0) {
        sonos_events_stop(); /* another group, or an address changed */
        snprintf(s_uuid, sizeof(s_uuid), "%s", room->uuid);
        snprintf(s_ip, sizeof(s_ip), "%s", room->ip);
        snprintf(s_callback, sizeof(s_callback), "%s", callback);
    }
    const int64_t now = esp_timer_get_time();
    if (now < s_renew_at) {
        return;
    }
    int granted = EV_TIMEOUT_S;
    bool all = true;
    for (size_t i = 0; i < EV_SERVICES; i++) {
        char sid[SONOS_SID_MAX];
        memcpy(sid, s_sid[i], sizeof(sid));
        int t = EV_TIMEOUT_S;
        esp_err_t err = sonos_subscribe(s_ip, SERVICE[i], s_callback, sid, sizeof(sid), &t);
        if (err != ESP_OK && sid[0]) {
            /* Not renewed (the speaker restarted, say): a new one. */
            sid[0] = '\0';
            t = EV_TIMEOUT_S;
            err = sonos_subscribe(s_ip, SERVICE[i], s_callback, sid, sizeof(sid), &t);
        }
        set_sid(i, err == ESP_OK ? sid : "");
        all &= err == ESP_OK;
        granted = t < granted ? t : granted;
    }
    if (all && !s_live) {
        ESP_LOGI(TAG, "Events from %s for %d s", s_ip, granted);
    } else if (!all) {
        ESP_LOGW(TAG, "No events from %s: polling", s_ip);
    }
    s_live = all;
    s_renew_at = now + (all ? (int64_t)(granted > 60 ? granted : 60) * 1000000LL / 2 : EV_RETRY_US);
}

bool sonos_events_live(void)
{
    return s_live;
}
