/* Spotify's Web API on the display: the login and the player (see
 * spotify_client.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "http_util.h"
#include "spotify_client.h"
#include "yr_client.h"

static const char *TAG = "spotify";

#define SP_API         "https://api.spotify.com/v1"
#define SP_TOKEN_URL   "https://accounts.spotify.com/api/token"
#define SP_AUTH_URL    "https://accounts.spotify.com/authorize"
/* The last two for the Sonos screen's "recently played" (main/sonos.c); a
 * login without them is asked to log in again there. */
#define SP_SCOPES      "user-read-playback-state user-modify-playback-state user-read-currently-playing " \
                       "user-read-recently-played playlist-read-private"
#define SP_NVS         "spotify"
#define SP_TOKEN_MAX   512
#define SP_CODE_MAX    1024 /* ~450 characters with the five scopes */
#define SP_TIMEOUT_MS  10000
#define SP_REPLY_MAX   (96 * 1024)  /* a player reply is 5-15 KB */
#define SP_IMAGE_MAX   (256 * 1024) /* a 300 px cover is 20-40 KB */
/* Recently played, a page at a time: ~3 KB of reply a track, and its cJSON
 * tree about twice that - 50 at once (~150 KB) was over SP_REPLY_MAX. */
#define SP_RECENT_PAGE  10
#define SP_RECENT_PAGES 4
#define SP_NAMES_MAX    24 /* playlists' names remembered */
#define SP_EARLY_US    (60 * 1000000LL) /* renew the access token this long before it runs out */

/* The login: shared between the web server (logging in, the setup page)
 * and the weather task (everything else), under s_mu. */
static SemaphoreHandle_t s_mu;
static char s_client_id[33];
static char s_redirect[160];
static char s_refresh[SP_TOKEN_MAX]; /* "" = not logged in */
static char s_user[64];
static spotify_login_t s_login;
static char s_verifier[65];          /* of the login under way */
static char s_state[128];            /* likewise; "" = none */
static EXT_RAM_BSS_ATTR char s_code[SP_CODE_MAX]; /* waiting for the weather task */

/* The weather task's own (off its 8 KB stack, which a TLS handshake needs). */
static char s_access[SP_TOKEN_MAX];
static char s_w_refresh[SP_TOKEN_MAX], s_w_fresh[SP_TOKEN_MAX];
static char s_w_auth[SP_TOKEN_MAX + 8];
static EXT_RAM_BSS_ATTR char s_w_code[SP_CODE_MAX];
static int64_t s_access_until_us;
static int s_retry_after_s;
static esp_http_client_handle_t s_client; /* kept alive to api.spotify.com */
static http_buf_t s_resp = { .max = SP_REPLY_MAX };

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static void nvs_put(const char *refresh, const char *user)
{
    nvs_handle_t h;
    if (nvs_open(SP_NVS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (refresh[0]) {
        nvs_set_str(h, "refresh", refresh);
        nvs_set_str(h, "user", user);
        nvs_set_str(h, "cid", s_client_id);
    } else {
        nvs_erase_all(h);
    }
    nvs_commit(h);
    nvs_close(h);
}

void spotify_init(const char *client_id, const char *redirect)
{
    s_mu = xSemaphoreCreateMutex();
    snprintf(s_client_id, sizeof(s_client_id), "%s", client_id);
    snprintf(s_redirect, sizeof(s_redirect), "%s", redirect);
    nvs_handle_t h;
    if (nvs_open(SP_NVS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "Not logged in");
        return;
    }
    char cid[sizeof(s_client_id)] = "";
    size_t len = sizeof(cid);
    nvs_get_str(h, "cid", cid, &len);
    len = sizeof(s_refresh);
    if (nvs_get_str(h, "refresh", s_refresh, &len) != ESP_OK) {
        s_refresh[0] = '\0';
    }
    len = sizeof(s_user);
    if (nvs_get_str(h, "user", s_user, &len) != ESP_OK) {
        s_user[0] = '\0';
    }
    nvs_close(h);
    if (s_refresh[0] && strcmp(cid, s_client_id) != 0) {
        ESP_LOGW(TAG, "Login made with another client ID: dropped");
        s_refresh[0] = '\0';
        nvs_put("", "");
    }
    s_login = s_refresh[0] ? SPOTIFY_CONNECTED : SPOTIFY_NOT_CONNECTED;
    ESP_LOGI(TAG, "%s%s", s_refresh[0] ? "Logged in as " : "Not logged in", s_refresh[0] ? s_user : "");
}

/* --------------------------------------------------------------------------
 * Logging in
 * ------------------------------------------------------------------------ */

static bool host_ok(const char *host)
{
    const size_t n = strlen(host);
    if (n == 0 || n > 80) {
        return false;
    }
    for (const char *c = host; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
              *c == '-' || *c == ':')) {
            return false;
        }
    }
    return true;
}

bool spotify_auth_url(const char *host, char *out, size_t out_len)
{
    if (s_client_id[0] == '\0' || s_redirect[0] == '\0' || !host_ok(host)) {
        return false;
    }
    /* The verifier: 64 of the unreserved characters; the challenge its
     * SHA-256 in base64url. */
    static const char CH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    char verifier[65];
    uint8_t rnd[64];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 64; i++) {
        verifier[i] = CH[rnd[i] % (sizeof(CH) - 1)];
    }
    verifier[64] = '\0';
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)verifier, 64, hash, 0);
    char challenge[48];
    spotify_base64url(hash, sizeof(hash), challenge);
    /* The state: a nonce, and where the update site's page is to send the
     * browser back to. */
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    char state[sizeof(s_state)];
    spotify_base64url(nonce, sizeof(nonce), state);
    snprintf(state + strlen(state), sizeof(state) - strlen(state), "~%s", host);

    char e_redirect[3 * sizeof(s_redirect)], e_scopes[3 * sizeof(SP_SCOPES)], e_state[3 * sizeof(state)];
    spotify_url_encode(s_redirect, e_redirect, sizeof(e_redirect));
    spotify_url_encode(SP_SCOPES, e_scopes, sizeof(e_scopes));
    spotify_url_encode(state, e_state, sizeof(e_state));
    const int n = snprintf(out, out_len,
                           SP_AUTH_URL "?response_type=code&client_id=%s&scope=%s&redirect_uri=%s"
                                       "&code_challenge_method=S256&code_challenge=%s&state=%s",
                           s_client_id, e_scopes, e_redirect, challenge, e_state);
    if (n < 0 || (size_t)n >= out_len) {
        return false;
    }
    lock();
    memcpy(s_verifier, verifier, sizeof(s_verifier));
    snprintf(s_state, sizeof(s_state), "%s", state);
    unlock();
    return true;
}

bool spotify_auth_return(const char *state, const char *code, const char *error)
{
    lock();
    const bool ours = s_state[0] != '\0' && strcmp(state, s_state) == 0;
    bool ok = false;
    if (ours) {
        s_state[0] = '\0'; /* once only */
        if (error[0] == '\0' && code[0] != '\0' && strlen(code) < sizeof(s_code)) {
            snprintf(s_code, sizeof(s_code), "%s", code);
            s_login = SPOTIFY_CONNECTING;
            ok = true;
        } else {
            s_login = s_refresh[0] ? SPOTIFY_CONNECTED : SPOTIFY_LOGIN_FAILED;
            ESP_LOGW(TAG, "Login refused: %s", error[0] ? error : "no code");
        }
    }
    unlock();
    if (!ours) {
        ESP_LOGW(TAG, "A login answer that wasn't asked for");
    }
    return ok;
}

spotify_login_t spotify_login_state(char *user, size_t user_len)
{
    lock();
    const spotify_login_t st = s_login;
    if (user != NULL) {
        snprintf(user, user_len, "%s", s_user);
    }
    unlock();
    return st;
}

void spotify_disconnect(void)
{
    lock();
    s_refresh[0] = s_user[0] = s_code[0] = '\0';
    s_login = SPOTIFY_NOT_CONNECTED;
    unlock();
    s_access_until_us = 0; /* read by the weather task: at worst one more request with it */
    nvs_put("", "");
    ESP_LOGI(TAG, "Logged out");
}

/* --------------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------------ */

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Retry-After") == 0) {
        s_retry_after_s = atoi(evt->header_value);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return http_buf_append(&s_resp, evt->data, evt->data_len, TAG);
    }
    return ESP_OK;
}

/* POST `form` to the token endpoint: the HTTP status, -1 if it wasn't
 * reached; the reply in s_resp. */
static int token_post(const char *form)
{
    http_buf_reset(&s_resp);
    esp_http_client_config_t cfg = {
        .url = SP_TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = on_event,
        .timeout_ms = SP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, form, (int)strlen(form));
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Token request failed: %s", esp_err_to_name(err));
    }
    return status;
}

/* Take the access token from a token reply in s_resp; a new refresh token
 * into refresh (if any). */
static bool take_tokens(char *refresh, size_t refresh_len)
{
    int expires = 0;
    json_use_psram();
    if (s_resp.buf == NULL ||
        !spotify_parse_token(s_resp.buf, s_access, sizeof(s_access), refresh, refresh_len, &expires)) {
        return false;
    }
    s_access_until_us = esp_timer_get_time() + (int64_t)expires * 1000000 - SP_EARLY_US;
    return true;
}

/* The login was withdrawn (the user removed the app's access, or the
 * client ID's app is gone). */
static void lose_login(void)
{
    ESP_LOGW(TAG, "Login no longer accepted: %.200s", s_resp.buf ? s_resp.buf : "");
    spotify_disconnect();
}

/* A current access token, renewed with the refresh token if needed. */
static esp_err_t ensure_access(void)
{
    if (s_access[0] && esp_timer_get_time() < s_access_until_us) {
        return ESP_OK;
    }
    char *const refresh = s_w_refresh, *const fresh = s_w_fresh;
    lock();
    snprintf(refresh, SP_TOKEN_MAX, "%s", s_refresh);
    unlock();
    s_access[0] = '\0';
    if (refresh[0] == '\0') {
        return SPOTIFY_ERR_LOGIN;
    }
    const size_t cap = 3 * SP_TOKEN_MAX + 128;
    char *form = malloc(cap);
    if (form == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(form, cap, "client_id=%s&grant_type=refresh_token&refresh_token=", s_client_id);
    spotify_url_encode(refresh, form + n, cap - (size_t)n);
    const int status = token_post(form);
    free(form);
    if (status == 200 && take_tokens(fresh, SP_TOKEN_MAX)) {
        if (fresh[0] && strcmp(fresh, refresh) != 0) {
            /* A new refresh token replaces the old one. */
            char user[sizeof(s_user)];
            lock();
            const bool same = strcmp(s_refresh, refresh) == 0; /* not logged out or in meanwhile */
            if (same) {
                snprintf(s_refresh, sizeof(s_refresh), "%s", fresh);
            }
            snprintf(user, sizeof(user), "%s", s_user);
            unlock();
            if (same) {
                nvs_put(fresh, user);
            }
        }
        return ESP_OK;
    }
    if (status == 400 || status == 401) {
        lose_login();
        return SPOTIFY_ERR_LOGIN;
    }
    ESP_LOGW(TAG, "Token renewal: HTTP %d", status);
    return ESP_FAIL;
}

void spotify_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    http_buf_free(&s_resp); /* grown for the biggest reply; not held for later */
}

/* One request to the API; the HTTP status (-1 if the server wasn't
 * reached), the reply in s_resp. */
static int api_once(esp_http_client_method_t method, const char *path, const char *body, bool *reused)
{
    http_buf_reset(&s_resp);
    s_retry_after_s = 0;
    *reused = s_client != NULL;
    char url[256];
    snprintf(url, sizeof(url), SP_API "%s", path);
    if (s_client == NULL) {
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = on_event,
            .timeout_ms = SP_TIMEOUT_MS,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return -1;
        }
    } else {
        esp_http_client_set_url(s_client, url);
    }
    snprintf(s_w_auth, sizeof(s_w_auth), "Bearer %s", s_access);
    esp_http_client_set_method(s_client, method);
    esp_http_client_set_header(s_client, "Authorization", s_w_auth);
    if (body != NULL) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, body, (int)strlen(body));
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, NULL, 0);
    }
    const esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", path, esp_err_to_name(err));
        spotify_close();
        return -1;
    }
    return esp_http_client_get_status_code(s_client);
}

/* A request with a current access token: renewed and tried again once if
 * Spotify says it ran out, and tried again on a fresh connection if a
 * kept-alive one was dropped. The HTTP status, or <0 with *err set. */
static int api(esp_http_client_method_t method, const char *path, const char *body, esp_err_t *err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if ((*err = ensure_access()) != ESP_OK) {
            return -1;
        }
        bool reused;
        const int64_t started = esp_timer_get_time();
        int status = api_once(method, path, body, &reused);
        if (status == -1 && http_retry_worthwhile(reused, started)) {
            status = api_once(method, path, body, &reused);
        }
        if (status == 401 && attempt == 0) {
            s_access[0] = '\0'; /* ran out early: renew */
            continue;
        }
        *err = ESP_OK;
        if (status == -1) {
            *err = ESP_FAIL;
        } else if (status == 401) {
            *err = SPOTIFY_ERR_LOGIN;
        } else if (status == 429) {
            *err = SPOTIFY_ERR_RATE;
            ESP_LOGW(TAG, "Asked too often: wait %d s", s_retry_after_s);
        }
        return status;
    }
    *err = SPOTIFY_ERR_LOGIN;
    return -1;
}

void spotify_auth_service(void)
{
    char *const code = s_w_code, *const refresh = s_w_fresh;
    char verifier[sizeof(s_verifier)];
    lock();
    snprintf(code, SP_CODE_MAX, "%s", s_code);
    snprintf(verifier, sizeof(verifier), "%s", s_verifier);
    s_code[0] = '\0';
    unlock();
    if (code[0] == '\0') {
        return;
    }
    const size_t cap = 3 * SP_CODE_MAX + 3 * sizeof(s_redirect) + 256;
    char *form = malloc(cap);
    if (form == NULL) {
        return;
    }
    int n = snprintf(form, cap, "client_id=%s&grant_type=authorization_code&code_verifier=%s&code=", s_client_id,
                     verifier);
    spotify_url_encode(code, form + n, cap - (size_t)n);
    n += (int)strlen(form + n);
    n += snprintf(form + n, cap - (size_t)n, "&redirect_uri=");
    spotify_url_encode(s_redirect, form + n, cap - (size_t)n);
    const int status = token_post(form);
    free(form);
    if (status != 200 || !take_tokens(refresh, SP_TOKEN_MAX) || refresh[0] == '\0') {
        ESP_LOGW(TAG, "Login failed: HTTP %d %.300s", status, s_resp.buf ? s_resp.buf : "");
        lock();
        s_login = s_refresh[0] ? SPOTIFY_CONNECTED : SPOTIFY_LOGIN_FAILED;
        unlock();
        return;
    }
    /* Whose account: the name shown on the setup page. */
    lock();
    snprintf(s_refresh, sizeof(s_refresh), "%s", refresh);
    unlock();
    char user[sizeof(s_user)] = "";
    esp_err_t err;
    if (api(HTTP_METHOD_GET, "/me", NULL, &err) == 200 && s_resp.buf != NULL) {
        cJSON *me = cJSON_Parse(s_resp.buf);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(me, "display_name");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(me, "id");
        snprintf(user, sizeof(user), "%s",
                 cJSON_IsString(name) && name->valuestring[0] ? name->valuestring
                 : cJSON_IsString(id)                         ? id->valuestring
                                                              : "");
        cJSON_Delete(me);
    }
    lock();
    snprintf(s_user, sizeof(s_user), "%s", user);
    s_login = SPOTIFY_CONNECTED;
    unlock();
    nvs_put(refresh, user);
    ESP_LOGI(TAG, "Logged in as %s", user);
}

/* --------------------------------------------------------------------------
 * The player
 * ------------------------------------------------------------------------ */

esp_err_t spotify_get_player(spotify_player_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_err_t err;
    const int status = api(HTTP_METHOD_GET, "/me/player?additional_types=episode", NULL, &err);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 204) {
        return ESP_OK; /* no device playing or paused */
    }
    json_use_psram();
    if (status != 200 || s_resp.buf == NULL || !spotify_parse_player(s_resp.buf, out)) {
        ESP_LOGW(TAG, "Player: HTTP %d %.200s", status, s_resp.buf ? s_resp.buf : "");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t spotify_get_devices(spotify_device_t *out, int max, int *count)
{
    *count = 0;
    esp_err_t err;
    const int status = api(HTTP_METHOD_GET, "/me/player/devices", NULL, &err);
    if (err != ESP_OK) {
        return err;
    }
    json_use_psram();
    const int n = status == 200 && s_resp.buf ? spotify_parse_devices(s_resp.buf, out, max) : -1;
    if (n < 0) {
        ESP_LOGW(TAG, "Devices: HTTP %d", status);
        return ESP_FAIL;
    }
    *count = n;
    return ESP_OK;
}

/* A playback command sent, and Spotify's answer as an error. */
static esp_err_t command(esp_http_client_method_t m, const char *path, const char *body)
{
    esp_err_t err;
    const int status = api(m, path, body, &err);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 200 || status == 202 || status == 204) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "%s: HTTP %d %.200s", path, status, s_resp.buf ? s_resp.buf : "");
    if (status == 403 && s_resp.buf && strstr(s_resp.buf, "PREMIUM_REQUIRED")) {
        return SPOTIFY_ERR_PREMIUM;
    }
    if (status == 403 && s_resp.buf && strstr(s_resp.buf, "Restricted device")) {
        return SPOTIFY_ERR_RESTRICTED;
    }
    if (status == 404) {
        return SPOTIFY_ERR_NO_DEVICE;
    }
    return ESP_FAIL;
}

esp_err_t spotify_command(spotify_cmd_t cmd, int value, const char *device_id)
{
    char path[96];
    char body[96];
    const char *b = NULL;
    esp_http_client_method_t m = HTTP_METHOD_PUT;
    switch (cmd) {
    case SPOTIFY_PLAY:
        snprintf(path, sizeof(path), "/me/player/play");
        break;
    case SPOTIFY_PAUSE:
        snprintf(path, sizeof(path), "/me/player/pause");
        break;
    case SPOTIFY_NEXT:
        snprintf(path, sizeof(path), "/me/player/next");
        m = HTTP_METHOD_POST;
        break;
    case SPOTIFY_PREVIOUS:
        snprintf(path, sizeof(path), "/me/player/previous");
        m = HTTP_METHOD_POST;
        break;
    case SPOTIFY_VOLUME:
        snprintf(path, sizeof(path), "/me/player/volume?volume_percent=%d", value < 0 ? 0 : value > 100 ? 100 : value);
        break;
    case SPOTIFY_TRANSFER:
        snprintf(path, sizeof(path), "/me/player");
        snprintf(body, sizeof(body), "{\"device_ids\":[\"%.48s\"],\"play\":true}", device_id ? device_id : "");
        b = body;
        break;
    case SPOTIFY_SHUFFLE:
        snprintf(path, sizeof(path), "/me/player/shuffle?state=%s", value ? "true" : "false");
        break;
    case SPOTIFY_REPEAT:
        snprintf(path, sizeof(path), "/me/player/repeat?state=%s",
                 value == SPOTIFY_REPEAT_CONTEXT ? "context"
                 : value == SPOTIFY_REPEAT_TRACK ? "track"
                                                 : "off");
        break;
    case SPOTIFY_SEEK:
        snprintf(path, sizeof(path), "/me/player/seek?position_ms=%d", value < 0 ? 0 : value);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return command(m, path, b);
}

esp_err_t spotify_play_context(const char *uri, const char *device_id)
{
    if (strncmp(uri, "spotify:", 8) != 0 || strlen(uri) > 64 || strpbrk(uri, "\"\\") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[96], body[96];
    snprintf(path, sizeof(path), "/me/player/play%s%.48s", device_id && device_id[0] ? "?device_id=" : "",
             device_id ? device_id : "");
    snprintf(body, sizeof(body), "{\"context_uri\":\"%s\"}", uri);
    ESP_LOGI(TAG, "Playing %s%s%s", uri, device_id && device_id[0] ? " on " : "", device_id ? device_id : "");
    return command(HTTP_METHOD_PUT, path, body);
}

/* A 401 or 403 for a permission the login lacks. */
static bool missing_scope(int status)
{
    return (status == 401 || status == 403) && s_resp.buf &&
           (strstr(s_resp.buf, "scope") || strstr(s_resp.buf, "ermission"));
}

esp_err_t spotify_get_recent(spotify_recent_t *out, int max, int *count)
{
    *count = 0;
    char before[32] = "";
    for (int page = 0; page < SP_RECENT_PAGES && *count < max; page++) {
        char path[96];
        snprintf(path, sizeof(path), "/me/player/recently-played?limit=%d%s%s", SP_RECENT_PAGE,
                 before[0] ? "&before=" : "", before);
        esp_err_t err;
        const int status = api(HTTP_METHOD_GET, path, NULL, &err);
        if (missing_scope(status)) {
            return SPOTIFY_ERR_SCOPE;
        }
        if (err != ESP_OK) {
            return page > 0 ? ESP_OK : err; /* what the pages before gave */
        }
        json_use_psram();
        const int n = status == 200 && s_resp.buf
                          ? spotify_parse_recent(s_resp.buf, out, *count, max, before, sizeof(before))
                          : -1;
        if (n < 0) {
            ESP_LOGW(TAG, "Recently played: HTTP %d %.200s", status, s_resp.buf ? s_resp.buf : "");
            return page > 0 ? ESP_OK : ESP_FAIL;
        }
        *count = n;
        if (before[0] == '\0') {
            break;
        }
    }
    return ESP_OK;
}

esp_err_t spotify_get_playlist(const char *uri, char *name, size_t name_len, char *image_url, size_t image_len)
{
    const char *id = strncmp(uri, "spotify:playlist:", 17) == 0 ? uri + 17 : "";
    if (id[0] == '\0' || strlen(id) > 40) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[96];
    snprintf(path, sizeof(path), "/playlists/%s?fields=name,images", id);
    esp_err_t err;
    const int status = api(HTTP_METHOD_GET, path, NULL, &err);
    if (missing_scope(status)) {
        return SPOTIFY_ERR_SCOPE;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (status == 404 || status == 403) {
        return ESP_ERR_NOT_FOUND;
    }
    json_use_psram();
    if (status != 200 || s_resp.buf == NULL || !spotify_parse_playlist(s_resp.buf, name, name_len, image_url, image_len)) {
        ESP_LOGW(TAG, "Playlist %s: HTTP %d", id, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t spotify_get_playlists(spotify_recent_t *out, int max, int *count)
{
    *count = 0;
    char path[64];
    snprintf(path, sizeof(path), "/me/playlists?limit=%d", max < 1 ? 1 : max > 50 ? 50 : max);
    esp_err_t err;
    const int status = api(HTTP_METHOD_GET, path, NULL, &err);
    if (missing_scope(status)) {
        return SPOTIFY_ERR_SCOPE;
    }
    if (err != ESP_OK) {
        return err;
    }
    json_use_psram();
    const int n = status == 200 && s_resp.buf ? spotify_parse_playlists(s_resp.buf, out, max) : -1;
    if (n < 0) {
        ESP_LOGW(TAG, "Playlists: HTTP %d %.200s", status, s_resp.buf ? s_resp.buf : "");
        return ESP_FAIL;
    }
    *count = n;
    return ESP_OK;
}

void spotify_name_playlists(spotify_recent_t *list, int n, int budget)
{
    /* Names asked before (the weather task's). */
    static struct {
        char uri[64];
        char name[64];
        char image[256];
    } names[SP_NAMES_MAX] EXT_RAM_BSS_ATTR;
    static int next;
    for (int i = 0; i < n; i++) {
        spotify_recent_t *r = &list[i];
        if (r->name[0] || strncmp(r->uri, "spotify:playlist:", 17) != 0) {
            continue;
        }
        int k = 0;
        while (k < SP_NAMES_MAX && strcmp(names[k].uri, r->uri) != 0) {
            k++;
        }
        if (k == SP_NAMES_MAX) {
            if (budget <= 0) {
                continue;
            }
            budget--;
            char name[64] = "", image[256] = "";
            const esp_err_t err = spotify_get_playlist(r->uri, name, sizeof(name), image, sizeof(image));
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                continue; /* asked again next time */
            }
            k = next++ % SP_NAMES_MAX;
            snprintf(names[k].uri, sizeof(names[k].uri), "%s", r->uri);
            snprintf(names[k].name, sizeof(names[k].name), "%s", name); /* "" for Spotify's own */
            snprintf(names[k].image, sizeof(names[k].image), "%s", image);
        }
        snprintf(r->name, sizeof(r->name), "%s", names[k].name);
        if (names[k].image[0]) {
            snprintf(r->image_url, sizeof(r->image_url), "%s", names[k].image);
        }
    }
}

int spotify_retry_after_s(void)
{
    return s_retry_after_s > 0 ? s_retry_after_s : 30;
}

esp_err_t spotify_get_image(const char *url, uint8_t **data, size_t *len)
{
    http_buf_t b = { .max = SP_IMAGE_MAX };
    const esp_err_t err =
        http_get_buf(url, yr_client_user_agent(), SP_TIMEOUT_MS, true, &b, TAG);
    if (err != ESP_OK || b.buf == NULL) {
        http_buf_free(&b);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    *data = (uint8_t *)b.buf;
    *len = b.len;
    return ESP_OK;
}
