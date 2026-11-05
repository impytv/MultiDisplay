/* Error reports: POST /feil from the navigation page (nav_page.js) with the
 * browser's form - the screenshot it took and packed, what the user wrote
 * and the screen on show, as multipart/form-data. The display puts its own
 * /status and /log in front (fields the update site reads first, so they
 * can't be the browser's), signs the whole and sends it to feil/ on the
 * update site (server/feil/feil.py), answering with what the site said.
 *
 * The signature is ECDSA P-256 with a key the display made itself the
 * first time and keeps in NVS, over report_signed_text: the time and the
 * body's SHA-256. The site takes only signed reports, recent and not seen
 * before, and no longer lets other web pages send them; it doesn't know
 * the keys beforehand, so this stops random abuse, not a determined one
 * (anyone can make a key). The reading and sending run in a task of their
 * own, as TLS and the signing need more stack than the web server's, and
 * so the web server isn't held up while the site answers. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "cJSON.h"

#include "app.h"
#include "app_config.h"
#include "clock.h"
#include "diag.h"
#include "report.h"
#include "report_util.h"
#include "watchdog.h"
#include "wifi_provision.h"
#include "yr_client.h"

static const char *TAG = "report";

/* The browser's part: a packed screenshot is some 50 KB, an unpacked one
 * (a browser that can't pack) 1.1 MB. */
#define REPORT_MAX_IN (1536 * 1024)
#define REPORT_LOG_MAX (30 * 1024)
#define REPORT_TIMEOUT_MS 30000
#define REPORT_STACK 12288
#define REPORT_ANSWER_MAX 256
/* PSRAM left free in one piece beside a report's body. */
#define REPORT_PSRAM_LEFT (512 * 1024)

typedef struct {
    char *body;
    size_t len;
    char ctype[128];
    char url[APP_CONFIG_OTA_URL_MAX + 8];
    int status;  /* the site's, or 0 if it couldn't be reached */
    char answer[REPORT_ANSWER_MAX];
    size_t answer_len;
    httpd_req_t *req; /* the browser's, taken off the web server's task */
    char boundary[72];
} job_t;

static volatile bool s_busy;

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* The display's key from NVS, made and kept the first time. */
static bool key_load(mbedtls_pk_context *pk)
{
    nvs_handle_t h;
    if (nvs_open("feil", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t der[160];
    size_t len = sizeof(der);
    bool ok = nvs_get_blob(h, "key", der, &len) == ESP_OK &&
              mbedtls_pk_parse_key(pk, der, len, NULL, 0, rng, NULL) == 0;
    if (!ok) {
        mbedtls_pk_free(pk);
        mbedtls_pk_init(pk);
        int n = -1;
        if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, NULL) == 0) {
            n = mbedtls_pk_write_key_der(pk, der, sizeof(der)); /* at the end of der */
        }
        ok = n > 0 && nvs_set_blob(h, "key", der + sizeof(der) - n, (size_t)n) == ESP_OK && nvs_commit(h) == ESP_OK;
        ESP_LOGI(TAG, "New key for reports: %s", ok ? "kept" : "failed");
    }
    nvs_close(h);
    return ok;
}

/* base64 of `len` bytes into out ('\0'-ended); false if it doesn't fit. */
static bool b64(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    size_t n;
    return mbedtls_base64_encode((unsigned char *)out, out_len, &n, in, len) == 0;
}

static esp_err_t on_answer(esp_http_client_event_t *evt)
{
    job_t *j = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && j->answer_len + 1 < sizeof(j->answer)) {
        size_t k = sizeof(j->answer) - 1 - j->answer_len;
        k = k < (size_t)evt->data_len ? k : (size_t)evt->data_len;
        memcpy(j->answer + j->answer_len, evt->data, k);
        j->answer_len += k;
        j->answer[j->answer_len] = '\0';
    }
    return ESP_OK;
}

/* Sign the job's body and send it; the result in the job. */
static void send_report(job_t *j)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    char key_b64[180], sig_b64[112], when[24];
    uint8_t sha[32], msg_sha[32], spki[128], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    char msg[REPORT_SIGNED_MAX];
    size_t sig_len = 0;
    const int64_t now = (int64_t)time(NULL);
    bool ok = key_load(&pk);
    if (ok) {
        const int n = mbedtls_pk_write_pubkey_der(&pk, spki, sizeof(spki)); /* at the end of spki */
        mbedtls_sha256((const unsigned char *)j->body, j->len, sha, 0);
        report_signed_text(now, sha, msg);
        mbedtls_sha256((const unsigned char *)msg, strlen(msg), msg_sha, 0);
        ok = n > 0 && b64(spki + sizeof(spki) - n, (size_t)n, key_b64, sizeof(key_b64)) &&
             mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, msg_sha, sizeof(msg_sha), sig, sizeof(sig), &sig_len, rng,
                             NULL) == 0 &&
             b64(sig, sig_len, sig_b64, sizeof(sig_b64));
    }
    mbedtls_pk_free(&pk);
    if (!ok) {
        ESP_LOGE(TAG, "Couldn't sign the report");
        j->status = 500;
        return;
    }
    snprintf(when, sizeof(when), "%lld", (long long)now);
    const esp_http_client_config_t cfg = {
        .url = j->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = REPORT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_answer,
        .user_data = j,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        j->status = 500;
        return;
    }
    esp_http_client_set_header(c, "Content-Type", j->ctype);
    esp_http_client_set_header(c, "User-Agent", yr_client_user_agent());
    esp_http_client_set_header(c, "X-Feil-Nokkel", key_b64);
    esp_http_client_set_header(c, "X-Feil-Tid", when);
    esp_http_client_set_header(c, "X-Feil-Signatur", sig_b64);
    esp_http_client_set_post_field(c, j->body, (int)j->len);
    const esp_err_t err = esp_http_client_perform(c);
    j->status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "Report of %u bytes to %s: %s %d", (unsigned)j->len, j->url, esp_err_to_name(err), j->status);
}

/* One form part, `name` with `value`, into p (as far as end). */
static char *part(char *p, const char *end, const char *boundary, const char *name, const char *value, size_t len)
{
    const int n = snprintf(p, end - p, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n", boundary, name);
    if (n < 0 || n >= end - p || (size_t)(end - p - n) < len + 2) {
        return NULL;
    }
    p += n;
    memcpy(p, value, len);
    p += len;
    memcpy(p, "\r\n", 2);
    return p + 2;
}

static esp_err_t answer(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* The browser's form read, the report made and sent, and the site's answer
 * given back - all with the request taken off the web server's task
 * (httpd_req_async_handler_begin), which goes on serving others meanwhile. */
static void handle(job_t *j, httpd_req_t *req, const char *boundary)
{
    size_t log_len = 0;
    char *log = diag_log_text(&log_len);
    char *status = diag_status_json();
    const char *log_from = log ? log + (log_len > REPORT_LOG_MAX ? log_len - REPORT_LOG_MAX : 0) : "";
    log_len = log ? log_len - (size_t)(log_from - log) : 0;
    const size_t cap = req->content_len + (status ? strlen(status) : 0) + log_len + 512;
    /* An unpacked screenshot is 1.1 MB: not if that would leave too little
     * for the forecasts meanwhile (they parse in ~370 KB). */
    const size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    j->body = cap + REPORT_PSRAM_LEFT <= room ? heap_caps_malloc(cap, MALLOC_CAP_SPIRAM) : NULL;
    if (j->body == NULL) {
        ESP_LOGW(TAG, "No room for a report of %u bytes (largest free %u)", (unsigned)cap, (unsigned)room);
        if (log) {
            heap_caps_free(log);
        }
        cJSON_free(status);
        answer(req, "507 Insufficient Storage", "{\"feil\":\"minne\"}");
        return;
    }
    wd_psram_held((int)cap); /* given back below, the sending timed out */
    char *p = j->body, *end = j->body + cap;
    p = part(p, end, boundary, "status", status ? status : "", status ? strlen(status) : 0);
    if (p != NULL) {
        p = part(p, end, boundary, "logg", log_from, log_len);
    }
    if (log) {
        heap_caps_free(log);
    }
    cJSON_free(status);
    /* The browser's form after them, as it came. */
    size_t got = 0;
    int timeouts = 0;
    while (p != NULL && got < req->content_len) {
        const int r = httpd_req_recv(req, p + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (r <= 0) {
            p = NULL;
            break;
        }
        got += (size_t)r;
    }
    if (p == NULL) {
        answer(req, "400 Bad Request", "{\"feil\":\"mottak\"}");
    } else {
        j->len = (size_t)(p - j->body) + got;
        send_report(j);
        if (j->status == 0) {
            answer(req, "502 Bad Gateway", "{\"feil\":\"ingen kontakt\"}");
        } else {
            char st[16];
            snprintf(st, sizeof(st), "%d %s", j->status, j->status == 200 ? "OK" : "Feil");
            httpd_resp_set_status(req, st);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_send(req, j->answer, (ssize_t)j->answer_len);
        }
    }
    heap_caps_free(j->body);
    j->body = NULL;
    wd_psram_held(-(int)cap);
}

static void report_task(void *arg)
{
    job_t *j = arg;
    handle(j, j->req, j->boundary);
    httpd_req_async_handler_complete(j->req);
    heap_caps_free(j);
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t h_report(httpd_req_t *req)
{
    job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM);
    if (j == NULL) {
        return answer(req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
    }
    app_config_report_url(g_cfg->ota_url, j->url, sizeof(j->url));
    esp_err_t ret;
    if (j->url[0] == '\0') {
        ret = answer(req, "404 Not Found", "{\"feil\":\"ingen adresse\"}");
    } else if (!clock_is_set()) {
        ret = answer(req, "503 Service Unavailable", "{\"feil\":\"klokken\"}");
    } else if (req->content_len == 0 || req->content_len > REPORT_MAX_IN) {
        ret = answer(req, "413 Payload Too Large", "{\"feil\":\"for stor\"}");
    } else if (httpd_req_get_hdr_value_str(req, "Content-Type", j->ctype, sizeof(j->ctype)) != ESP_OK ||
               !report_boundary(j->ctype, j->boundary, sizeof(j->boundary))) {
        ret = answer(req, "400 Bad Request", "{\"feil\":\"ikke et skjema\"}");
    } else if (s_busy) {
        ret = answer(req, "429 Too Many Requests", "{\"feil\":\"opptatt\"}");
    } else if (httpd_req_async_handler_begin(req, &j->req) != ESP_OK) {
        ret = answer(req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
    } else {
        s_busy = true;
        if (xTaskCreate(report_task, "report", REPORT_STACK, j, 4, NULL) == pdPASS) {
            return ESP_OK; /* the task answers, completes the request and frees the job */
        }
        s_busy = false;
        ret = answer(j->req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
        httpd_req_async_handler_complete(j->req);
    }
    heap_caps_free(j);
    return ret;
}

void report_start(void)
{
    wifi_provision_add_post_handler("/feil", h_report);
}
