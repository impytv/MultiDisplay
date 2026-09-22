#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "met_alerts_client.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "met_alerts_client";

#define MET_ALERTS_HTTP_TIMEOUT_MS  15000
#define MET_ALERTS_MAX_RESPONSE_LEN (128 * 1024) /* safety cap against a runaway server */

/* cJSON's default allocator is plain malloc(), which ESP-IDF only routes to
 * PSRAM for allocations >= 1 KB (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) - an
 * alert's polygon geometry can run to hundreds of coordinate pairs, each a
 * small cJSON node well under that, so this can burn through internal DRAM
 * while parsed even though the properties we actually keep are tiny. Routing
 * cJSON straight to heap_caps_malloc(..., MALLOC_CAP_SPIRAM) sidesteps that
 * size threshold (see yr_client.c, which hit this for real with the larger
 * "complete" forecast product). Global to the cJSON library, hence the guard. */
static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void cjson_psram_free(void *ptr) { heap_caps_free(ptr); }

static void ensure_cjson_psram_hooks(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
}

typedef struct {
    char *buf;
    size_t len;
} met_alerts_response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    met_alerts_response_buf_t *resp = (met_alerts_response_buf_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    if (resp->len + evt->data_len + 1 > MET_ALERTS_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }

    /* PSRAM: this is a short-lived fetch buffer, no reason to compete with
     * mbedtls/esp_http_client for scarce internal DRAM (see yr_client.c). */
    char *new_buf = heap_caps_realloc(resp->buf, resp->len + evt->data_len + 1, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        ESP_LOGE(TAG, "Out of memory growing response buffer to %u bytes",
                 (unsigned)(resp->len + evt->data_len + 1));
        return ESP_FAIL;
    }
    resp->buf = new_buf;
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';

    return ESP_OK;
}

/* GET url into a freshly heap_caps_malloc'd (PSRAM) NUL-terminated buffer;
 * caller frees *body on success. Returns ESP_OK only on HTTP 200 with a body.
 * No crt_bundle_attach/cacert - see yr_client.c's yr_http_get for why. */
static esp_err_t met_alerts_http_get(const char *url, char **body)
{
    *body = NULL;

    met_alerts_response_buf_t resp = { .buf = NULL, .len = 0 };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MET_ALERTS_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status %d", status);
        free(resp.buf);
        return ESP_FAIL;
    }
    if (resp.buf == NULL) {
        return ESP_FAIL;
    }

    *body = resp.buf;
    return ESP_OK;
}

static met_alert_color_t parse_color(const char *s)
{
    if (s != NULL) {
        if (strcasecmp(s, "Red") == 0) {
            return MET_ALERT_RED;
        }
        if (strcasecmp(s, "Orange") == 0) {
            return MET_ALERT_ORANGE;
        }
    }
    return MET_ALERT_YELLOW; /* the mildest colour is also MetAlerts' fallback */
}

/* The response is a GeoJSON FeatureCollection; an empty "features" array is a
 * valid, successful result meaning nothing is active at this point right
 * now, not an error. */
static bool parse_alerts(const char *json, met_alerts_t *out)
{
    ensure_cjson_psram_hooks();
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    bool ok = false;
    cJSON *features = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (!cJSON_IsArray(features)) {
        ESP_LOGE(TAG, "No \"features\" array in response");
        goto done;
    }

    int n = 0;
    cJSON *feature = NULL;
    cJSON_ArrayForEach(feature, features)
    {
        if (n >= MET_ALERTS_MAX) {
            break;
        }

        cJSON *props = cJSON_GetObjectItemCaseSensitive(feature, "properties");
        if (props == NULL) {
            continue;
        }

        met_alert_t *a = &out->alerts[n];
        memset(a, 0, sizeof(*a));

        cJSON *name = cJSON_GetObjectItemCaseSensitive(props, "eventAwarenessName");
        if (cJSON_IsString(name)) {
            snprintf(a->event_name, sizeof(a->event_name), "%s", name->valuestring);
        }

        cJSON *area = cJSON_GetObjectItemCaseSensitive(props, "area");
        if (cJSON_IsString(area)) {
            snprintf(a->area, sizeof(a->area), "%s", area->valuestring);
        }

        cJSON *color = cJSON_GetObjectItemCaseSensitive(props, "riskMatrixColor");
        a->color = parse_color(cJSON_IsString(color) ? color->valuestring : NULL);

        n++;
    }

    out->count = n;
    out->valid = true;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t met_alerts_client_fetch(double lat, double lon, met_alerts_t *out)
{
    memset(out, 0, sizeof(*out));

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/metalerts/2.0/current.json?lat=%.4f&lon=%.4f",
             lat, lon);

    char *body = NULL;
    esp_err_t err = met_alerts_http_get(url, &body);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = parse_alerts(body, out);
    free(body);
    return ok ? ESP_OK : ESP_FAIL;
}
