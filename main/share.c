/* Pictures shared between displays, through the update site's deling/
 * (server/deling/deling.py).
 *
 * A display shares a picture on its card with a group ("familien") from
 * the /bilder page; displays in that group with "Last ned delte bilder"
 * on (app_config_t.share_dl) download it onto their own card as
 * D<id>.JPG, where the picture screen shows it like the rest, and delete it
 * again once it is no longer shared. Deleting one on the /bilder page keeps
 * it from coming back (SKJULT.TXT beside the pictures). Who shared each,
 * by the nickname that display chose (app_config_t.share_nick), is kept in
 * DELT.TXT beside them for the /bilder page.
 *
 * Only displays may share or download: every request is signed with the
 * display's own key (display_key.c) over share_signed_text - the method,
 * the address, the time and a one-off nonce, and the body's SHA-256 - and
 * the site takes it only from a key someone has approved there and put in
 * the groups, and only recent and never seen before. A key the site hasn't
 * seen asks to be approved ("venter") the first time.
 *
 * All of it runs in one task, started when there is something to do (a
 * page's request, or the downloads every SHARE_EVERY_S) and ending when
 * there isn't: TLS, signing and the card need more stack than the web
 * server's, and an internal stack - the card and NVS can't run from a
 * PSRAM one - is only taken while it runs. */

#include "share.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "app.h"
#include "app_config.h"
#include "clock.h"
#include "display_key.h"
#include "form_util.h"
#include "http_util.h"
#include "pic_util.h"
#include "pictures.h"
#include "sd_card.h"
#include "share_util.h"
#include "wifi_provision.h"
#include "yr_client.h"

static const char *TAG = "share";

#define SHARE_EVERY_S (30 * 60)     /* the downloads, this often */
#define SHARE_FIRST_S 120           /* the first after a start, once the clock is set */
#define SHARE_MORE_S 60             /* more left to download: again after this */
#define SHARE_RETRY_S (5 * 60)      /* the site or card wasn't there */
#define SHARE_BATCH 40              /* pictures downloaded in one go */
#define SHARE_LIST_MAX (256 * 1024) /* the site's list of pictures */
#define SHARE_IDS_MAX 3000          /* pictures in it, at most */
#define SHARE_ANSWER_MAX (64 * 1024)
#define SHARE_TIMEOUT_MS 20000
#define SHARE_STACK 12288
#define SHARE_IDLE_MS 10000 /* the task ends after this with nothing to do */
#define SHARE_HIDDEN PICTURES_DIR "/SKJULT.TXT"
#define SHARE_CARD_LEFT (64 * 1024) /* left free on the card beside a picture */

typedef enum { JOB_STATUS, JOB_SHARE, JOB_UNSHARE } job_kind_t;

typedef struct {
    job_kind_t kind;
    httpd_req_t *req; /* the browser's, taken off the web server's task */
    char name[PIC_NAME_MAX];
    char group[SHARE_GROUP_MAX];
    char id[SHARE_ID_LEN + 1];
} job_t;

static QueueHandle_t s_jobs;
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_running;     /* the task is (under s_mux) */
static bool s_sync_due;    /* the downloads are due (under s_mux) */
static int64_t s_synced_us; /* when they last ran, 0 = not yet */
static bool s_told;         /* the site has this start's name and nickname */

typedef char share_id_t[SHARE_ID_LEN + 1];
typedef char share_nick_t[SHARE_NICK_MAX];

/* ---- Asking the site ---- */

/* `method` of `path` (what follows deling/) with `body`, signed; the reply
 * in *out (up to out->max). The HTTP status, 0 if the site couldn't be
 * reached, -1 if the request couldn't be made. */
static int ask(const char *method, const char *path, const uint8_t *body, size_t body_len, http_buf_t *out)
{
    char base[APP_CONFIG_OTA_URL_MAX + 16];
    app_config_share_url(g_cfg->ota_url, base, sizeof(base));
    if (base[0] == '\0' || !clock_is_set()) {
        return -1;
    }
    char url[sizeof(base) + 320];
    if ((size_t)snprintf(url, sizeof(url), "%s%s", base, path) >= sizeof(url)) {
        return -1;
    }
    uint8_t rnd[16], sha[32], text_sha[32];
    char nonce[33], when[24], text[480], key_b64[180], sig_b64[112];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        snprintf(nonce + 2 * i, 3, "%02x", rnd[i]);
    }
    const int64_t now = (int64_t)time(NULL);
    mbedtls_sha256(body ? body : (const uint8_t *)"", body ? body_len : 0, sha, 0);
    const size_t n = share_signed_text(method, path, now, nonce, sha, text, sizeof(text));
    if (n == 0) {
        return -1;
    }
    mbedtls_sha256((const uint8_t *)text, n, text_sha, 0);
    if (!display_key_sign(text_sha, key_b64, sizeof(key_b64), sig_b64, sizeof(sig_b64))) {
        ESP_LOGE(TAG, "Couldn't sign");
        return -1;
    }
    snprintf(when, sizeof(when), "%lld", (long long)now);
    const bool https = strncmp(url, "https://", 8) == 0;
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = SHARE_TIMEOUT_MS,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    esp_http_client_set_header(c, "User-Agent", yr_client_user_agent());
    esp_http_client_set_header(c, "X-Deling-Nokkel", key_b64);
    esp_http_client_set_header(c, "X-Deling-Tid", when);
    esp_http_client_set_header(c, "X-Deling-Nonce", nonce);
    esp_http_client_set_header(c, "X-Deling-Signatur", sig_b64);
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    }
    int status = 0;
    out->len = 0;
    if (esp_http_client_open(c, body ? (int)body_len : 0) == ESP_OK) {
        bool ok = true;
        for (size_t sent = 0; body && sent < body_len && ok;) {
            const int w = esp_http_client_write(c, (const char *)body + sent, (int)(body_len - sent));
            ok = w > 0;
            sent += ok ? (size_t)w : 0;
        }
        const int64_t cl = ok ? esp_http_client_fetch_headers(c) : -1;
        status = ok && cl >= 0 ? esp_http_client_get_status_code(c) : 0;
        /* Room for all of it at once when the site says how much. */
        if (status > 0 && cl > 0 && (size_t)cl < out->max && (size_t)cl + 1 > out->cap) {
            char *nb = heap_caps_realloc(out->buf, (size_t)cl + 1, MALLOC_CAP_SPIRAM);
            if (nb != NULL) {
                out->buf = nb;
                out->cap = (size_t)cl + 1;
            }
        }
        char chunk[1024];
        int r;
        while (status > 0 && (r = esp_http_client_read(c, chunk, sizeof(chunk))) > 0) {
            if (http_buf_append(out, chunk, (size_t)r, TAG) != ESP_OK) {
                status = -1;
            }
        }
        if (status > 0 && !esp_http_client_is_complete_data_received(c)) {
            status = 0;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status == 0) {
        ESP_LOGW(TAG, "%s %s: no answer", method, path);
    } else if (status != 200) {
        ESP_LOGW(TAG, "%s %s: %d %.80s", method, path, status, out->len ? out->buf : "");
    }
    return status;
}

/* ---- The pictures hidden on this display ---- */

void share_hide(const char *id)
{
    FILE *f = fopen(SHARE_HIDDEN, "a");
    if (f != NULL) {
        fprintf(f, "%s\n", id);
        fclose(f);
    }
}

/* SKJULT.TXT's ids into ids (at most max); how many (SD lock held). */
static int hidden_read(share_id_t *ids, int max)
{
    FILE *f = fopen(SHARE_HIDDEN, "r");
    if (f == NULL) {
        return 0;
    }
    int n = 0;
    char line[32];
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (share_id_ok(line)) {
            memcpy(ids[n++], line, sizeof(share_id_t)); /* 12 digits and the '\0' */
        }
    }
    fclose(f);
    return n;
}

static bool has(share_id_t *ids, int n, const char *id)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- The downloads ---- */

/* The ids in the site's list that aren't this display's own, into ids
 * (SHARE_IDS_MAX), and who shared each into nicks; how many, or -1 if it
 * isn't a list. */
static int parse_list(const char *json, share_id_t *ids, share_nick_t *nicks)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "bilder");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(root, "navn");
    if (!cJSON_IsArray(names)) {
        names = NULL;
    }
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, list)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (n < SHARE_IDS_MAX && cJSON_IsString(id) && share_id_ok(id->valuestring) &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "mitt"))) {
            /* The sharer's nickname: its place in the list's "navn". */
            const cJSON *fra = cJSON_GetObjectItemCaseSensitive(e, "fra");
            const cJSON *nick = cJSON_IsNumber(fra) && fra->valueint >= 0 ? cJSON_GetArrayItem(names, fra->valueint) : NULL;
            share_nick_clean(cJSON_IsString(nick) ? nick->valuestring : "", nicks[n], sizeof(share_nick_t));
            snprintf(ids[n++], sizeof(share_id_t), "%s", id->valuestring);
        }
    }
    cJSON_Delete(root);
    return n;
}

/* The shared pictures on the card into on (PICTURES_MAX), and how many
 * pictures there are in all into *all; how many shared, -1 if the card
 * can't be read (SD lock held). */
static int card_shared(share_id_t *on, int *all)
{
    DIR *d = opendir(PICTURES_DIR);
    if (d == NULL) {
        return -1;
    }
    int n = 0;
    *all = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        share_id_t id;
        *all += pic_is_picture(e->d_name);
        if (n < PICTURES_MAX && share_file_id(e->d_name, id)) {
            snprintf(on[n++], sizeof(share_id_t), "%s", id);
        }
    }
    closedir(d);
    return n;
}

/* DELT.TXT made "<id>\t<nick>\n" for those of the site's pictures on the
 * card (have) that someone put a nickname on, if it isn't that already
 * (SD lock held). */
static void who_write(share_id_t *ids, share_nick_t *nicks, const uint8_t *have, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        len += have[i] && nicks[i][0] ? SHARE_ID_LEN + 2 + strlen(nicks[i]) : 0;
    }
    char *want = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    char *was = heap_caps_malloc(len + 2, MALLOC_CAP_SPIRAM);
    if (want == NULL || was == NULL) {
        heap_caps_free(want);
        heap_caps_free(was);
        return;
    }
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        if (have[i] && nicks[i][0]) {
            k += (size_t)snprintf(want + k, len + 1 - k, "%s\t%s\n", ids[i], nicks[i]);
        }
    }
    FILE *f = fopen(SHARE_WHO_FILE, "r");
    const size_t got = f ? fread(was, 1, len + 1, f) : 0; /* one more: longer is different too */
    if (f) {
        fclose(f);
    }
    if ((f != NULL || len > 0) && (got != len || memcmp(was, want, len) != 0)) {
        if (len == 0) {
            unlink(SHARE_WHO_FILE);
        } else {
            f = fopen(SHARE_WHO_FILE ".NY", "w");
            bool ok = f != NULL && fwrite(want, 1, len, f) == len;
            ok = f != NULL && fclose(f) == 0 && ok;
            if (ok) {
                unlink(SHARE_WHO_FILE);
                rename(SHARE_WHO_FILE ".NY", SHARE_WHO_FILE);
            }
        }
    }
    heap_caps_free(want);
    heap_caps_free(was);
}

/* The path of meg: the display's name and nickname, for the site. */
static void me_path(char *out, size_t len)
{
    char name[3 * APP_CONFIG_DEVNAME_MAX], nick[3 * SHARE_NICK_MAX];
    share_url_encode(g_cfg->device_name[0] ? g_cfg->device_name : "V\xC3\xA6rtavla", name, sizeof(name));
    share_url_encode(g_cfg->share_nick, nick, sizeof(nick));
    snprintf(out, len, "meg?navn=%s&kallenavn=%s", name, nick);
}

/* Bring the card's shared pictures in line with the site's list. The
 * seconds until it should run again. */
static int sync_pictures(void)
{
    /* A nickname changed on the setup page (which restarts) reaches the
     * site without the /bilder page being opened. Not by a display that
     * neither downloads nor has a nickname: it hasn't asked to share. */
    if (!s_told && (g_cfg->share_dl || g_cfg->share_nick[0])) {
        char me[300];
        me_path(me, sizeof(me));
        http_buf_t m = { .max = SHARE_ANSWER_MAX };
        s_told = ask("GET", me, NULL, 0, &m) == 200;
        http_buf_free(&m);
    }
    if (!g_cfg->share_dl) {
        return SHARE_EVERY_S;
    }
    char path[32 + 3 * SHARE_GROUPS_MAX];
    char groups[3 * SHARE_GROUPS_MAX];
    share_url_encode(g_cfg->share_groups, groups, sizeof(groups));
    snprintf(path, sizeof(path), "bilder?grupper=%s", groups);
    http_buf_t b = { .max = SHARE_LIST_MAX };
    const int st = ask("GET", path, NULL, 0, &b);
    if (st != 200) {
        http_buf_free(&b);
        return st == 403 ? SHARE_EVERY_S : SHARE_RETRY_S; /* not approved (yet) */
    }
    share_id_t *site = heap_caps_malloc(sizeof(share_id_t) * SHARE_IDS_MAX, MALLOC_CAP_SPIRAM);
    share_id_t *on = heap_caps_malloc(sizeof(share_id_t) * PICTURES_MAX, MALLOC_CAP_SPIRAM);
    share_id_t *hidden = heap_caps_malloc(sizeof(share_id_t) * PICTURES_MAX, MALLOC_CAP_SPIRAM);
    share_nick_t *nicks = heap_caps_malloc(sizeof(share_nick_t) * SHARE_IDS_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *have = heap_caps_calloc(SHARE_IDS_MAX, 1, MALLOC_CAP_SPIRAM);
    const int n_site = site && on && hidden && nicks && have ? parse_list(b.buf, site, nicks) : -1;
    http_buf_free(&b);
    if (n_site < 0) {
        heap_caps_free(site);
        heap_caps_free(on);
        heap_caps_free(hidden);
        heap_caps_free(nicks);
        heap_caps_free(have);
        return SHARE_RETRY_S;
    }

    /* What is no longer shared, off the card; the hidden ones still
     * shared kept in SKJULT.TXT, the rest left out. */
    int all = 0, n_on = -1, n_hidden = 0, removed = 0;
    sd_card_lock();
    if (sd_card_ready() == ESP_OK && (n_on = card_shared(on, &all)) >= 0) {
        for (int i = 0; i < n_on; i++) {
            if (!has(site, n_site, on[i])) {
                char name[PIC_NAME_MAX], p[sizeof(PICTURES_DIR) + PIC_NAME_MAX];
                share_file_name(on[i], name, sizeof(name));
                snprintf(p, sizeof(p), PICTURES_DIR "/%s", name);
                if (unlink(p) == 0) {
                    removed++;
                    all--;
                    ESP_LOGI(TAG, "Deleted %s: no longer shared", name);
                }
                on[i][0] = '\0';
            }
        }
        const int n_read = hidden_read(hidden, PICTURES_MAX);
        for (int i = 0; i < n_read; i++) {
            if (has(site, n_site, hidden[i]) && !has(hidden, n_hidden, hidden[i])) {
                if (n_hidden != i) {
                    memcpy(hidden[n_hidden], hidden[i], sizeof(share_id_t));
                }
                n_hidden++;
            }
        }
        if (n_hidden != n_read) {
            FILE *f = fopen(SHARE_HIDDEN ".NY", "w");
            bool ok = f != NULL;
            for (int i = 0; ok && i < n_hidden; i++) {
                ok = fprintf(f, "%s\n", hidden[i]) > 0;
            }
            ok = f != NULL && fclose(f) == 0 && ok;
            if (ok) {
                unlink(SHARE_HIDDEN);
                rename(SHARE_HIDDEN ".NY", SHARE_HIDDEN);
            }
        }
    }
    sd_card_unlock();

    /* What is new, onto it: a few at a time, the card held only while one
     * is saved. */
    int got = 0, left = 0, wait = SHARE_EVERY_S;
    bool paused = false;
    http_buf_t pic = { .max = PICTURES_UPLOAD_MAX + 1 };
    for (int i = 0; n_on >= 0 && i < n_site; i++) {
        have[i] = has(on, n_on, site[i]);
    }
    for (int i = 0; n_on >= 0 && i < n_site; i++) {
        if (have[i] || has(hidden, n_hidden, site[i])) {
            continue;
        }
        /* A page's request waiting goes first: the rest in a minute. */
        paused |= uxQueueMessagesWaiting(s_jobs) > 0;
        if (got >= SHARE_BATCH || all >= PICTURES_MAX || paused) {
            left++;
            continue;
        }
        if (!pictures_psram_room(PICTURES_UPLOAD_MAX)) {
            ESP_LOGW(TAG, "No room in memory to download pictures now");
            wait = SHARE_RETRY_S;
            break;
        }
        char q[32];
        snprintf(q, sizeof(q), "bilde?id=%s", site[i]);
        const int s = ask("GET", q, NULL, 0, &pic);
        if (s != 200 || pic.len < 4 || (uint8_t)pic.buf[0] != 0xFF || (uint8_t)pic.buf[1] != 0xD8) {
            wait = SHARE_RETRY_S;
            if (s == 0 || s >= 500) {
                break;
            }
            continue;
        }
        char name[PIC_NAME_MAX];
        share_file_name(site[i], name, sizeof(name));
        uint64_t total = 0, free_b = 0;
        sd_card_lock();
        bool ok = sd_card_ready() == ESP_OK;
        if (ok && sd_card_space(&total, &free_b) == ESP_OK && free_b < pic.len + SHARE_CARD_LEFT) {
            sd_card_unlock();
            ESP_LOGW(TAG, "The card is full: no more shared pictures downloaded");
            left += n_site - i;
            break; /* tried again at the usual time */
        }
        if (ok) {
            ok = pictures_save(name, (const uint8_t *)pic.buf, pic.len);
            if (!ok) {
                sd_card_failed();
            }
        }
        sd_card_unlock();
        if (!ok) {
            wait = SHARE_RETRY_S;
            break;
        }
        got++;
        all++;
        have[i] = 1;
        ESP_LOGI(TAG, "Downloaded %s (%u bytes)", name, (unsigned)pic.len);
        if (got % 5 == 0) {
            pictures_changed(); /* shown as they come */
        }
    }
    http_buf_free(&pic);
    if (n_on >= 0) {
        sd_card_lock();
        if (sd_card_ready() == ESP_OK) {
            who_write(site, nicks, have, n_site);
        }
        sd_card_unlock();
    }
    heap_caps_free(site);
    heap_caps_free(on);
    heap_caps_free(hidden);
    heap_caps_free(nicks);
    heap_caps_free(have);
    if (got || removed) {
        pictures_changed();
        app_wake_poll();
        ESP_LOGI(TAG, "Shared pictures: %d downloaded, %d deleted, %d left", got, removed, left);
    }
    return left > 0 && wait == SHARE_EVERY_S && (got > 0 || paused) ? SHARE_MORE_S : wait;
}

/* ---- The /bilder page's requests ---- */

static void answer(httpd_req_t *req, const char *status, const char *json)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
}

/* The site's error as the page knows it ({"feil": ...}). */
static void answer_site(httpd_req_t *req, int st, const http_buf_t *b)
{
    if (st == 0) {
        answer(req, "502 Bad Gateway", "{\"feil\":\"ingen kontakt\"}");
    } else if (st < 0) {
        answer(req, "503 Service Unavailable", clock_is_set() ? "{\"feil\":\"ingen adresse\"}" : "{\"feil\":\"klokken\"}");
    } else if (st == 403) {
        answer(req, "403 Forbidden", "{\"feil\":\"ikke godkjent\"}");
    } else if (st == 413) {
        answer(req, "413 Payload Too Large", "{\"feil\":\"for stor\"}");
    } else if (st == 429) {
        answer(req, "429 Too Many Requests", "{\"feil\":\"for mange\"}");
    } else if (st == 507) {
        answer(req, "507 Insufficient Storage", "{\"feil\":\"fullt\"}");
    } else {
        answer(req, "502 Bad Gateway", "{\"feil\":\"siden\"}");
    }
}

/* GET /bilder/deling.json: whether the site has approved the display, its
 * groups, the pictures it shares, and the settings. */
static void job_status(job_t *j)
{
    char path[300];
    me_path(path, sizeof(path));
    http_buf_t b = { .max = SHARE_ANSWER_MAX };
    int st = ask("GET", path, NULL, 0, &b);
    s_told |= st == 200;
    cJSON *me = st == 200 ? cJSON_Parse(b.buf) : NULL;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(me, "status");
    if (!cJSON_IsString(status)) {
        cJSON_Delete(me);
        answer_site(j->req, st == 200 ? 502 : st, &b);
        http_buf_free(&b);
        return;
    }
    cJSON_AddBoolToObject(me, "auto", g_cfg->share_dl);
    cJSON_AddStringToObject(me, "fra", g_cfg->share_groups);
    char kid[17];
    if (display_key_id(kid)) {
        cJSON_AddStringToObject(me, "nokkel", kid);
    }
    if (strcmp(status->valuestring, "godkjent") == 0) {
        st = ask("GET", "mine", NULL, 0, &b);
        cJSON *mine = st == 200 ? cJSON_Parse(b.buf) : NULL;
        cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(mine, "bilder");
        cJSON_AddItemToObject(me, "mine", cJSON_IsArray(list) ? list : cJSON_CreateArray());
        if (!cJSON_IsArray(list)) {
            cJSON_Delete(list);
        }
        cJSON_Delete(mine);
    }
    http_buf_free(&b);
    char *json = cJSON_PrintUnformatted(me);
    cJSON_Delete(me);
    answer(j->req, NULL, json ? json : "{}");
    cJSON_free(json);
}

/* POST /bilder/del: picture n on the card shared with group gruppe. */
static void job_share(job_t *j)
{
    size_t len = 0;
    sd_card_lock();
    uint8_t *jpg = sd_card_ready() == ESP_OK ? pictures_read(j->name, &len) : NULL;
    sd_card_unlock();
    if (jpg == NULL) {
        answer(j->req, "404 Not Found", "{\"feil\":\"finnes ikke\"}");
        return;
    }
    char path[48 + 3 * PIC_NAME_MAX], name[3 * PIC_NAME_MAX];
    share_url_encode(j->name, name, sizeof(name));
    snprintf(path, sizeof(path), "del?gruppe=%s&navn=%s", j->group, name);
    http_buf_t b = { .max = SHARE_ANSWER_MAX };
    const int st = ask("POST", path, jpg, len, &b);
    heap_caps_free(jpg);
    if (st == 200) {
        ESP_LOGI(TAG, "Shared %s with %s", j->name, j->group);
        answer(j->req, NULL, b.buf);
    } else {
        answer_site(j->req, st, &b);
    }
    http_buf_free(&b);
}

/* POST /bilder/slutt-a-dele: shared picture id no longer shared. */
static void job_unshare(job_t *j)
{
    char path[32];
    snprintf(path, sizeof(path), "slett?id=%s", j->id);
    http_buf_t b = { .max = SHARE_ANSWER_MAX };
    const int st = ask("POST", path, (const uint8_t *)"", 0, &b);
    if (st == 200 || st == 404) {
        ESP_LOGI(TAG, "No longer sharing %s", j->id);
        answer(j->req, NULL, "{}");
    } else {
        answer_site(j->req, st, &b);
    }
    http_buf_free(&b);
}

/* ---- The task ---- */

static void share_task(void *arg)
{
    for (;;) {
        job_t *j = NULL;
        if (xQueueReceive(s_jobs, &j, s_sync_due ? 0 : pdMS_TO_TICKS(SHARE_IDLE_MS)) == pdTRUE) {
            if (j->kind == JOB_STATUS) {
                job_status(j);
            } else if (j->kind == JOB_SHARE) {
                job_share(j);
            } else {
                job_unshare(j);
            }
            httpd_req_async_handler_complete(j->req);
            heap_caps_free(j);
            continue;
        }
        taskENTER_CRITICAL(&s_mux);
        const bool due = s_sync_due;
        s_sync_due = false;
        taskEXIT_CRITICAL(&s_mux);
        if (due) {
            const int again = sync_pictures();
            s_synced_us = esp_timer_get_time();
            esp_timer_stop(s_timer);
            esp_timer_start_once(s_timer, (uint64_t)again * 1000000ull);
            continue;
        }
        taskENTER_CRITICAL(&s_mux);
        const bool done = uxQueueMessagesWaiting(s_jobs) == 0 && !s_sync_due;
        if (done) {
            s_running = false;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (done) {
            /* Of SHARE_STACK, with TLS and the signing on it: worth
             * seeing in /log now and then. */
            ESP_LOGI(TAG, "Done; stack left %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            vTaskDelete(NULL);
        }
    }
}

/* The task started if it isn't running. False if it couldn't be. */
static bool kick(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool start = !s_running;
    s_running = true;
    taskEXIT_CRITICAL(&s_mux);
    if (start && xTaskCreate(share_task, "share", SHARE_STACK, NULL, 3, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_mux);
        s_running = false;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "No memory for the task");
        return false;
    }
    return true;
}

static void on_timer(void *arg)
{
    taskENTER_CRITICAL(&s_mux);
    s_sync_due = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!kick()) {
        esp_timer_start_once(s_timer, (uint64_t)SHARE_RETRY_S * 1000000ull);
    }
}

/* A page's request handed to the task, which answers it. */
static esp_err_t queue_job(httpd_req_t *req, job_t *j)
{
    if (httpd_req_async_handler_begin(req, &j->req) != ESP_OK) {
        heap_caps_free(j);
        answer(req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
        return ESP_OK;
    }
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        answer(j->req, "429 Too Many Requests", "{\"feil\":\"opptatt\"}");
        httpd_req_async_handler_complete(j->req);
        heap_caps_free(j);
        return ESP_OK;
    }
    if (!kick()) {
        /* Left in the queue for the next start; the page waits for it. */
        ESP_LOGW(TAG, "A request waits for the task");
    }
    return ESP_OK;
}

static job_t *new_job(httpd_req_t *req, job_kind_t kind)
{
    job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM);
    if (j == NULL) {
        answer(req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
    } else {
        j->kind = kind;
    }
    return j;
}

/* The form in a small POST body into buf; false if it isn't one. */
static bool small_form(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len == 0 || req->content_len >= len) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t h_status(httpd_req_t *req)
{
    job_t *j = new_job(req, JOB_STATUS);
    if (j == NULL) {
        return ESP_OK;
    }
    /* The downloads, too, if they haven't run for a while: the page is
     * often opened just after the setting was turned on. */
    if (g_cfg->share_dl && (s_synced_us == 0 || esp_timer_get_time() - s_synced_us > 5 * 60 * 1000000ll)) {
        taskENTER_CRITICAL(&s_mux);
        s_sync_due = true;
        taskEXIT_CRITICAL(&s_mux);
    }
    return queue_job(req, j);
}

static esp_err_t h_share(httpd_req_t *req)
{
    char body[4 * PIC_NAME_MAX + 64];
    job_t *j = new_job(req, JOB_SHARE);
    if (j == NULL) {
        return ESP_OK;
    }
    share_id_t id;
    if (!small_form(req, body, sizeof(body)) || !form_field(body, "n", j->name, sizeof(j->name)) ||
        !pictures_name_ok(j->name) || share_file_id(j->name, id) || /* not one shared from another */
        !form_field(body, "gruppe", j->group, sizeof(j->group)) || !share_group_ok(j->group)) {
        heap_caps_free(j);
        answer(req, "400 Bad Request", "{\"feil\":\"skjema\"}");
        return ESP_OK;
    }
    return queue_job(req, j);
}

static esp_err_t h_unshare(httpd_req_t *req)
{
    char body[64];
    job_t *j = new_job(req, JOB_UNSHARE);
    if (j == NULL) {
        return ESP_OK;
    }
    if (!small_form(req, body, sizeof(body)) || !form_field(body, "id", j->id, sizeof(j->id)) ||
        !share_id_ok(j->id)) {
        heap_caps_free(j);
        answer(req, "400 Bad Request", "{\"feil\":\"skjema\"}");
        return ESP_OK;
    }
    return queue_job(req, j);
}

void share_start(void)
{
    s_jobs = xQueueCreate(4, sizeof(job_t *));
    const esp_timer_create_args_t t = { .callback = on_timer, .name = "share" };
    if (s_jobs == NULL || esp_timer_create(&t, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't start");
        return;
    }
    esp_timer_start_once(s_timer, (uint64_t)SHARE_FIRST_S * 1000000ull);
    wifi_provision_add_get_handler("/bilder/deling.json", h_status);
    wifi_provision_add_post_handler("/bilder/del", h_share);
    wifi_provision_add_post_handler("/bilder/slutt-a-dele", h_unshare);
}
