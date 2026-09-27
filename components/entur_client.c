#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "entur_client.h"
#include "civil_time.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "entur_client";

#define ENTUR_JP_URL          "https://api.entur.io/journey-planner/v3/graphql"
/* Entur asks every client to identify itself as "<company>-<application>". */
#define ENTUR_CLIENT_NAME     "trondve-multidisplay"
#define ENTUR_HTTP_TIMEOUT_MS 15000
/* ~450 bytes per call; at most ENTUR_MAX_STOPS * 100 calls. */
#define ENTUR_MAX_RESPONSE_LEN (512 * 1024)

/* cJSON in PSRAM - the response is hundreds of small nodes (see
 * met_alerts_client.c). Global to the cJSON library, hence the guard. */
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

/* --------------------------------------------------------------------------
 * Selection text
 * ------------------------------------------------------------------------ */

/* Entur IDs are "CODESPACE:Type:Value"; allowing only these characters also
 * keeps them safe to put straight into the GraphQL query text. */
static bool id_char_ok(char c)
{
    return isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-' || c == '.';
}

/* Copy [s, e) trimmed of spaces into dst; false if empty, too long or with a
 * character an ID can't have. */
static bool take_id(const char *s, const char *e, char *dst, size_t dst_len)
{
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t n = (size_t)(e - s);
    if (n == 0 || n >= dst_len) {
        return false;
    }
    for (const char *c = s; c < e; c++) {
        if (!id_char_ok(*c)) {
            return false;
        }
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
    return true;
}

static const char *find_in(const char *s, const char *e, char c)
{
    const char *p = memchr(s, c, (size_t)(e - s));
    return p ? p : e;
}

bool entur_parse_selection(const char *text, entur_selection_t *out)
{
    memset(out, 0, sizeof(*out));
    if (text == NULL) {
        return false;
    }
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end) {
        const char *stop_end = find_in(p, end, ';');
        const char *eq = find_in(p, stop_end, '=');
        if (out->stop_count >= ENTUR_MAX_STOPS) {
            ESP_LOGW(TAG, "more than %d stops; the rest are ignored", ENTUR_MAX_STOPS);
            break;
        }
        entur_stop_sel_t *st = &out->stops[out->stop_count];
        char id[ENTUR_ID_MAX];
        if (take_id(p, eq, id, sizeof(id))) {
            bool digits = true;
            for (const char *c = id; *c; c++) {
                digits &= isdigit((unsigned char)*c) != 0;
            }
            if (digits) {
                snprintf(st->stop_id, sizeof(st->stop_id), "NSR:StopPlace:%.25s", id);
            } else {
                snprintf(st->stop_id, sizeof(st->stop_id), "%s", id);
            }
            const char *l = (eq < stop_end) ? eq + 1 : stop_end;
            while (l < stop_end) {
                const char *l_end = find_in(l, stop_end, ',');
                const char *slash = find_in(l, l_end, '/');
                entur_line_sel_t line = { 0 };
                if (take_id(l, slash, line.id, sizeof(line.id))) {
                    if (slash < l_end) {
                        char dir[8];
                        if (take_id(slash + 1, l_end, dir, sizeof(dir))) {
                            if (strcasecmp(dir, "in") == 0 || strcasecmp(dir, "inbound") == 0) {
                                line.dirs = ENTUR_DIR_IN;
                            } else if (strcasecmp(dir, "out") == 0 || strcasecmp(dir, "outbound") == 0) {
                                line.dirs = ENTUR_DIR_OUT;
                            }
                        }
                    }
                    if (st->line_count < ENTUR_MAX_LINES) {
                        st->lines[st->line_count++] = line;
                    } else {
                        ESP_LOGW(TAG, "%s: more than %d lines; %s ignored", st->stop_id, ENTUR_MAX_LINES, line.id);
                    }
                } else if (l_end > l) {
                    ESP_LOGW(TAG, "%s: bad line entry '%.*s'", st->stop_id, (int)(l_end - l), l);
                }
                l = l_end + 1;
            }
            out->stop_count++;
        } else if (eq > p) {
            ESP_LOGW(TAG, "bad stop entry '%.*s'", (int)(stop_end - p), p);
        }
        p = stop_end + 1;
    }
    return out->stop_count > 0;
}

/* --------------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static resp_buf_t s_resp;
static esp_http_client_handle_t s_client;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (s_resp.len + evt->data_len + 1 > ENTUR_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }
    if (s_resp.len + evt->data_len + 1 > s_resp.cap) {
        size_t new_cap = s_resp.cap ? s_resp.cap * 2 : 16 * 1024;
        while (new_cap < s_resp.len + evt->data_len + 1) {
            new_cap *= 2;
        }
        char *nb = heap_caps_realloc(s_resp.buf, new_cap, MALLOC_CAP_SPIRAM);
        if (nb == NULL) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %u", (unsigned)new_cap);
            return ESP_FAIL;
        }
        s_resp.buf = nb;
        s_resp.cap = new_cap;
    }
    memcpy(s_resp.buf + s_resp.len, evt->data, evt->data_len);
    s_resp.len += evt->data_len;
    s_resp.buf[s_resp.len] = '\0';
    return ESP_OK;
}

void entur_client_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

/* One POST of `body`; returns the HTTP status, or -1 on transport error. */
static int post_query(const char *body)
{
    s_resp.len = 0;
    if (s_resp.buf != NULL) {
        s_resp.buf[0] = '\0';
    }
    if (s_client == NULL) {
        esp_http_client_config_t cfg = {
            .url = ENTUR_JP_URL,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event_handler,
            .timeout_ms = ENTUR_HTTP_TIMEOUT_MS,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return -1;
        }
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_header(s_client, "ET-Client-Name", ENTUR_CLIENT_NAME);
    }
    esp_http_client_set_post_field(s_client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        entur_client_close();
        return -1;
    }
    return esp_http_client_get_status_code(s_client);
}

/* --------------------------------------------------------------------------
 * Query and response
 * ------------------------------------------------------------------------ */

#define CALL_FIELDS                                                               \
    "realtime cancellation aimedDepartureTime expectedDepartureTime "             \
    "destinationDisplay{frontText} quay{publicCode} "                             \
    "serviceJourney{directionType line{id publicCode transportMode "              \
    "presentation{colour textColour}}}"

/* The GraphQL text: one aliased stopPlace per stop (s0, s1, ...). NULL if it
 * doesn't fit. */
static char *build_query(const entur_selection_t *sel)
{
    const size_t cap = 4096;
    char *q = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (q == NULL) {
        return NULL;
    }
    size_t n = snprintf(q, cap, "{");
    for (int i = 0; i < sel->stop_count && n < cap; i++) {
        const entur_stop_sel_t *st = &sel->stops[i];
        /* Enough calls to find the next two departures per line and direction. */
        int calls = st->line_count ? 20 * st->line_count : 100;
        if (calls > 100) {
            calls = 100;
        }
        n += snprintf(q + n, cap - n,
                      "s%d:stopPlace(id:\"%s\"){name estimatedCalls(timeRange:86400,numberOfDepartures:%d,"
                      "arrivalDeparture:departures,includeCancelledTrips:true",
                      i, st->stop_id, calls);
        if (st->line_count && n < cap) {
            n += snprintf(q + n, cap - n, ",whiteListed:{lines:[");
            for (int l = 0; l < st->line_count && n < cap; l++) {
                n += snprintf(q + n, cap - n, "%s\"%s\"", l ? "," : "", st->lines[l].id);
            }
            if (n < cap) {
                n += snprintf(q + n, cap - n, "]}");
            }
        }
        if (n < cap) {
            n += snprintf(q + n, cap - n, "){" CALL_FIELDS "}}");
        }
    }
    if (n < cap) {
        n += snprintf(q + n, cap - n, "}");
    }
    if (n >= cap) {
        ESP_LOGE(TAG, "query too long");
        free(q);
        return NULL;
    }
    return q;
}

/* ISO 8601 with offset ("2026-09-27T20:31:00+02:00" or "...Z") to epoch
 * seconds; 0 if it doesn't parse. */
static int64_t parse_time(const char *s)
{
    int y, mo, d, h, mi, se, n = 0;
    if (s == NULL || sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6) {
        return 0;
    }
    const char *z = s + n;
    if (*z == '.') { /* fractional seconds */
        z++;
        while (isdigit((unsigned char)*z)) z++;
    }
    int off = 0;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        off = (oh * 60 + om) * 60 * (*z == '-' ? -1 : 1);
    }
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
}

static const char *str_at(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool parse_hex_colour(const char *s, uint32_t *out)
{
    if (s == NULL || strlen(s) != 6) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (*end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Line numbers in natural order: "2" < "10" < "10E" < "N12". */
static int compare_codes(const char *a, const char *b)
{
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
        long na = strtol(a, NULL, 10), nb = strtol(b, NULL, 10);
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
    }
    return strcmp(a, b);
}

static int compare_groups(const void *pa, const void *pb)
{
    const entur_group_t *a = pa, *b = pb;
    if (a->stop != b->stop) {
        return a->stop < b->stop ? -1 : 1;
    }
    int c = compare_codes(a->code, b->code);
    if (c != 0) {
        return c;
    }
    return (a->calls[0].expected > b->calls[0].expected) - (a->calls[0].expected < b->calls[0].expected);
}

typedef struct {
    char line_id[ENTUR_ID_MAX];
    char dir[40];
} group_key_t;

/* Keep the call unless its line is listed with other directions only. */
static bool direction_wanted(const entur_stop_sel_t *st, const char *line_id, const char *dir_type)
{
    for (int i = 0; i < st->line_count; i++) {
        if (strcmp(st->lines[i].id, line_id) == 0) {
            if (st->lines[i].dirs == 0) {
                return true;
            }
            uint8_t d = (dir_type && strcmp(dir_type, "inbound") == 0) ? ENTUR_DIR_IN
                      : (dir_type && strcmp(dir_type, "outbound") == 0) ? ENTUR_DIR_OUT : 0;
            return d == 0 || (st->lines[i].dirs & d) != 0;
        }
    }
    return true;
}

/* Group the calls of stop `si` per line + direction into `out`, the first
 * ENTUR_PER_GROUP calls of each (the API lists them in departure order). */
static void add_stop_calls(const cJSON *calls, const entur_stop_sel_t *st, int si,
                           entur_departures_t *out, group_key_t *keys)
{
    const cJSON *call;
    cJSON_ArrayForEach(call, calls)
    {
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(call, "serviceJourney");
        const cJSON *line = cJSON_GetObjectItemCaseSensitive(sj, "line");
        const char *line_id = str_at(line, "id");
        if (line_id == NULL) {
            continue;
        }
        const char *dir_type = str_at(sj, "directionType");
        if (!direction_wanted(st, line_id, dir_type)) {
            continue;
        }
        const char *front = str_at(cJSON_GetObjectItemCaseSensitive(call, "destinationDisplay"), "frontText");
        const char *dir = (dir_type && strcmp(dir_type, "unknown") != 0) ? dir_type : (front ? front : "");

        int g;
        for (g = 0; g < out->group_count; g++) {
            if (out->groups[g].stop == si && strcmp(keys[g].line_id, line_id) == 0 &&
                strncmp(keys[g].dir, dir, sizeof(keys[g].dir) - 1) == 0) {
                break;
            }
        }
        if (g == out->group_count) {
            if (out->group_count >= ENTUR_MAX_GROUPS) {
                continue;
            }
            out->group_count++;
            entur_group_t *ng = &out->groups[g];
            memset(ng, 0, sizeof(*ng));
            snprintf(keys[g].line_id, sizeof(keys[g].line_id), "%s", line_id);
            snprintf(keys[g].dir, sizeof(keys[g].dir), "%s", dir);
            ng->stop = (uint8_t)si;
            const char *code = str_at(line, "publicCode");
            const char *mode = str_at(line, "transportMode");
            snprintf(ng->code, sizeof(ng->code), "%s", code ? code : "?");
            snprintf(ng->mode, sizeof(ng->mode), "%s", mode ? mode : "");
            snprintf(ng->dest, sizeof(ng->dest), "%s", front ? front : "");
            const char *quay = str_at(cJSON_GetObjectItemCaseSensitive(call, "quay"), "publicCode");
            snprintf(ng->quay, sizeof(ng->quay), "%s", quay ? quay : "");
            const cJSON *pres = cJSON_GetObjectItemCaseSensitive(line, "presentation");
            ng->has_colour = parse_hex_colour(str_at(pres, "colour"), &ng->colour);
            if (!parse_hex_colour(str_at(pres, "textColour"), &ng->text_colour)) {
                ng->text_colour = 0xFFFFFF;
            }
        }
        entur_group_t *grp = &out->groups[g];
        if (grp->call_count >= ENTUR_PER_GROUP) {
            continue;
        }
        entur_call_t *c = &grp->calls[grp->call_count++];
        c->expected = parse_time(str_at(call, "expectedDepartureTime"));
        c->aimed = parse_time(str_at(call, "aimedDepartureTime"));
        c->realtime = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(call, "realtime"));
        c->cancelled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(call, "cancellation"));
    }
}

static bool parse_response(const char *json, const entur_selection_t *sel, entur_departures_t *out)
{
    ensure_cjson_psram_hooks();
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }
    bool ok = false;
    group_key_t *keys = heap_caps_calloc(ENTUR_MAX_GROUPS, sizeof(*keys), MALLOC_CAP_SPIRAM);
    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(errors) && cJSON_GetArraySize(errors) > 0) {
        const char *msg = str_at(cJSON_GetArrayItem(errors, 0), "message");
        ESP_LOGE(TAG, "Journey Planner: %s", msg ? msg : "error");
    }
    if (keys == NULL || !cJSON_IsObject(data)) {
        goto done;
    }

    out->stop_count = sel->stop_count;
    for (int i = 0; i < sel->stop_count; i++) {
        char alias[16];
        snprintf(alias, sizeof(alias), "s%d", i);
        const cJSON *sp = cJSON_GetObjectItemCaseSensitive(data, alias);
        const char *name = str_at(sp, "name");
        snprintf(out->stop_name[i], sizeof(out->stop_name[i]), "%s", name ? name : sel->stops[i].stop_id);
        if (sp == NULL || cJSON_IsNull(sp)) {
            ESP_LOGW(TAG, "stop %s not found", sel->stops[i].stop_id);
            continue;
        }
        add_stop_calls(cJSON_GetObjectItemCaseSensitive(sp, "estimatedCalls"), &sel->stops[i], i, out, keys);
    }
    qsort(out->groups, out->group_count, sizeof(out->groups[0]), compare_groups);
    out->valid = true;
    ok = true;

done:
    free(keys);
    cJSON_Delete(root);
    return ok;
}

esp_err_t entur_client_fetch(const entur_selection_t *sel, entur_departures_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sel->stop_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *query = build_query(sel);
    if (query == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ensure_cjson_psram_hooks();
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "query", query);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(query);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = post_query(body);
    if (status == -1) {
        status = post_query(body); /* kept-alive connection dropped by the server: reconnect once */
    }
    cJSON_free(body);
    if (status != 200 || s_resp.buf == NULL) {
        ESP_LOGW(TAG, "fetch failed: status=%d %.300s", status, s_resp.buf ? s_resp.buf : "");
        entur_client_close();
        return ESP_FAIL;
    }
    if (!parse_response(s_resp.buf, sel, out)) {
        ESP_LOGW(TAG, "unparseable response (%u bytes)", (unsigned)s_resp.len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%d stop(s), %d line rows (%u bytes)", out->stop_count, out->group_count,
             (unsigned)s_resp.len);
    return ESP_OK;
}
