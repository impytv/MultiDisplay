/* Sonos speakers on the display (see sonos_client.h): finding them, and
 * their UPnP control on port 1400. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "http_util.h"
#include "sonos_client.h"
#include "yr_client.h"

static const char *TAG = "sonos";

#define SO_TIMEOUT_MS   5000
#define SO_REPLY_MAX    (96 * 1024)  /* the favourites (~1.5 KB each) or the rooms */
#define SO_FAVS_ASKED   "30"         /* favourites asked for: some can't be started (Sonos Radio's) */
#define SO_IMAGE_MAX    (256 * 1024) /* a speaker's 640 px cover is ~90 KB */
#define SO_SSDP_MS      1500
/* Spotify as a Sonos service: 2311 in most places, 3079 in some. */
static const int SPOTIFY_SERVICES[] = { 2311, 3079 };

/* The weather task's. */
static http_buf_t s_resp = { .max = SO_REPLY_MAX };
static int s_fault;              /* the last reply's UPnP error, 0 = none */
static char s_last_ip[16];       /* a speaker that answered */
static int s_spotify_service;    /* the one Sonos took, 0 = not known yet */

/* The rooms found last, for the setup page (under s_mu). */
static SemaphoreHandle_t s_mu;
static char s_known[SONOS_ROOMS_MAX][48];
static int s_known_count;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return http_buf_append(&s_resp, evt->data, evt->data_len, TAG);
    }
    return ESP_OK;
}

/* One action of `service` on the speaker at `ip`, with `args` (XML, already
 * escaped): ESP_OK on HTTP 200, the reply in s_resp; a fault's code in
 * s_fault. */
static esp_err_t soap(const char *ip, const char *service, const char *action, const char *args)
{
    static const struct {
        const char *service, *path;
    } PATHS[] = {
        { "AVTransport", "/MediaRenderer/AVTransport/Control" },
        { "RenderingControl", "/MediaRenderer/RenderingControl/Control" },
        { "GroupRenderingControl", "/MediaRenderer/GroupRenderingControl/Control" },
        { "ContentDirectory", "/MediaServer/ContentDirectory/Control" },
        { "ZoneGroupTopology", "/ZoneGroupTopology/Control" },
    };
    const char *path = NULL;
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        if (strcmp(PATHS[i].service, service) == 0) {
            path = PATHS[i].path;
        }
    }
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t cap = strlen(args) + 512;
    char *body = malloc(cap);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, cap,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
             "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
             "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%s></s:Body></s:Envelope>",
             action, service, args, action);
    char url[80], soap_action[96];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, path);
    snprintf(soap_action, sizeof(soap_action), "\"urn:schemas-upnp-org:service:%s:1#%s\"", service, action);
    http_buf_reset(&s_resp);
    s_fault = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = on_event,
        .timeout_ms = SO_TIMEOUT_MS,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(c, "SOAPACTION", soap_action);
    esp_http_client_set_post_field(c, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    free(body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s at %s: %s", action, ip, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        s_fault = sonos_parse_fault(s_resp.buf);
        ESP_LOGW(TAG, "%s at %s: HTTP %d, error %d", action, ip, status, s_fault);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* s_resp's body copied (each SOAP call reuses s_resp); NULL if none. */
static char *take_reply(void)
{
    return s_resp.buf ? strdup(s_resp.buf) : NULL;
}

/* --------------------------------------------------------------------------
 * Finding them
 * ------------------------------------------------------------------------ */

/* The first speaker to answer an SSDP search, into ip. */
static bool ssdp_find(char *ip, size_t ip_len)
{
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return false;
    }
    const struct timeval tv = { .tv_sec = 0, .tv_usec = 300 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    static const char MSG[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                              "MX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1900) };
    inet_aton("239.255.255.250", &to.sin_addr);
    bool found = false;
    char buf[512];
    for (int tries = 0; tries < SO_SSDP_MS / 300 && !found; tries++) {
        if (tries % 2 == 0) {
            sendto(s, MSG, sizeof(MSG) - 1, 0, (struct sockaddr *)&to, sizeof(to));
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "ZonePlayer") || strstr(buf, "Sonos")) {
                inet_ntoa_r(from.sin_addr, ip, ip_len);
                found = true;
            }
        }
    }
    close(s);
    return found;
}

static esp_err_t rooms_from(const char *ip, sonos_room_t *out, int max, int *count)
{
    esp_err_t err = soap(ip, "ZoneGroupTopology", "GetZoneGroupState", "");
    if (err != ESP_OK) {
        return err;
    }
    const int n = sonos_parse_rooms(s_resp.buf, out, max);
    if (n < 0) {
        ESP_LOGW(TAG, "Rooms: not a reply from %s", ip);
        return ESP_FAIL;
    }
    *count = n;
    return ESP_OK;
}

esp_err_t sonos_get_rooms(sonos_room_t *out, int max, int *count)
{
    *count = 0;
    esp_err_t err = ESP_FAIL;
    if (s_last_ip[0]) {
        err = rooms_from(s_last_ip, out, max, count);
    }
    if (err != ESP_OK) {
        char ip[16];
        if (!ssdp_find(ip, sizeof(ip))) {
            ESP_LOGW(TAG, "No speaker answered");
            return ESP_ERR_NOT_FOUND;
        }
        if ((err = rooms_from(ip, out, max, count)) != ESP_OK) {
            return err;
        }
        if (strcmp(ip, s_last_ip) != 0) {
            ESP_LOGI(TAG, "Found %d rooms through %s", *count, ip);
        }
        snprintf(s_last_ip, sizeof(s_last_ip), "%s", ip);
    }
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (s_mu != NULL && xSemaphoreTake(s_mu, portMAX_DELAY)) {
        s_known_count = *count;
        for (int i = 0; i < *count; i++) {
            snprintf(s_known[i], sizeof(s_known[i]), "%.47s", out[i].name);
        }
        xSemaphoreGive(s_mu);
    }
    return ESP_OK;
}

int sonos_known_rooms(char (*names)[48], int max)
{
    int n = 0;
    if (s_mu != NULL && xSemaphoreTake(s_mu, portMAX_DELAY)) {
        for (; n < s_known_count && n < max; n++) {
            snprintf(names[n], 48, "%.47s", s_known[n]);
        }
        xSemaphoreGive(s_mu);
    }
    return n;
}

/* --------------------------------------------------------------------------
 * Playing
 * ------------------------------------------------------------------------ */

#define INSTANCE "<InstanceID>0</InstanceID>"

esp_err_t sonos_get_now(const sonos_room_t *room, sonos_now_t *out)
{
    static const char *const ACTIONS[] = { "GetTransportInfo", "GetPositionInfo", "GetMediaInfo",
                                           "GetCurrentTransportActions" };
    char *r[4] = { 0 };
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 4 && err == ESP_OK; i++) {
        if ((err = soap(room->ip, "AVTransport", ACTIONS[i], INSTANCE)) == ESP_OK && (r[i] = take_reply()) == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        out->volume = soap(room->ip, "GroupRenderingControl", "GetGroupVolume", INSTANCE) == ESP_OK
                          ? sonos_parse_volume(s_resp.buf)
                          : -1;
        if (!sonos_parse_now(r[0], r[1], r[2], r[3], room->ip, out)) {
            ESP_LOGW(TAG, "%s: not a reply", room->name);
            err = ESP_FAIL;
        }
    }
    for (int i = 0; i < 4; i++) {
        free(r[i]);
    }
    return err;
}

esp_err_t sonos_get_state(const sonos_room_t *room, sonos_state_t *out)
{
    const esp_err_t err = soap(room->ip, "AVTransport", "GetTransportInfo", INSTANCE);
    if (err != ESP_OK) {
        return err;
    }
    return sonos_parse_state(s_resp.buf, out) ? ESP_OK : ESP_FAIL;
}

esp_err_t sonos_join(const sonos_room_t *room, const char *coord_uuid)
{
    ESP_LOGI(TAG, "%s joins %s", room->name, coord_uuid);
    char args[160];
    snprintf(args, sizeof(args), INSTANCE "<CurrentURI>x-rincon:%.31s</CurrentURI><CurrentURIMetaData>"
                                 "</CurrentURIMetaData>", coord_uuid);
    return soap(room->ip, "AVTransport", "SetAVTransportURI", args);
}

esp_err_t sonos_leave(const sonos_room_t *room)
{
    ESP_LOGI(TAG, "%s leaves its group", room->name);
    return soap(room->ip, "AVTransport", "BecomeCoordinatorOfStandaloneGroup", INSTANCE);
}

esp_err_t sonos_command(const sonos_room_t *room, sonos_cmd_t cmd, int value)
{
    switch (cmd) {
    case SONOS_PLAY:
        return soap(room->ip, "AVTransport", "Play", INSTANCE "<Speed>1</Speed>");
    case SONOS_PAUSE:
        return soap(room->ip, "AVTransport", "Pause", INSTANCE);
    case SONOS_STOP:
        return soap(room->ip, "AVTransport", "Stop", INSTANCE);
    case SONOS_NEXT:
        return soap(room->ip, "AVTransport", "Next", INSTANCE);
    case SONOS_PREVIOUS:
        return soap(room->ip, "AVTransport", "Previous", INSTANCE);
    case SONOS_VOLUME: {
        /* The snapshot keeps the speakers' volumes relative to each other. */
        soap(room->ip, "GroupRenderingControl", "SnapshotGroupVolume", INSTANCE);
        char args[80];
        snprintf(args, sizeof(args), INSTANCE "<DesiredVolume>%d</DesiredVolume>",
                 value < 0 ? 0 : value > 100 ? 100 : value);
        return soap(room->ip, "GroupRenderingControl", "SetGroupVolume", args);
    }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t sonos_get_favorites(const sonos_room_t *room, sonos_fav_t *out, int max, int *count)
{
    *count = 0;
    const esp_err_t err = soap(room->ip, "ContentDirectory", "Browse",
                               "<ObjectID>FV:2</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
                               "<Filter>*</Filter><StartingIndex>0</StartingIndex>"
                               "<RequestedCount>" SO_FAVS_ASKED "</RequestedCount><SortCriteria></SortCriteria>");
    if (err != ESP_OK) {
        return err;
    }
    const int n = sonos_parse_favorites(s_resp.buf, room->ip, out, max);
    if (n < 0) {
        return ESP_FAIL;
    }
    *count = n;
    return ESP_OK;
}

/* `uri` with `meta` (neither escaped yet) as an action's arguments:
 * "<uri_tag>..</uri_tag><meta_tag>..</meta_tag>", after `before`, before
 * `after`. NULL without memory. */
static char *uri_args(const char *before, const char *uri_tag, const char *uri, const char *meta_tag,
                      const char *meta, const char *after)
{
    const size_t ul = 6 * strlen(uri) + 1, ml = 6 * strlen(meta) + 1;
    const size_t cap = strlen(before) + ul + ml + strlen(after) + 128;
    char *args = malloc(cap), *eu = malloc(ul), *em = malloc(ml);
    if (args && eu && em && sonos_xml_escape(uri, eu, ul) && sonos_xml_escape(meta, em, ml)) {
        snprintf(args, cap, "%s<%s>%s</%s><%s>%s</%s>%s", before, uri_tag, eu, uri_tag, meta_tag, em, meta_tag,
                 after);
    } else {
        free(args);
        args = NULL;
    }
    free(eu);
    free(em);
    return args;
}

/* The room's queue replaced by `uri` (a container or a track), played. */
static esp_err_t play_queued(const sonos_room_t *room, const char *uri, const char *meta)
{
    char *args = uri_args(INSTANCE, "EnqueuedURI", uri, "EnqueuedURIMetaData", meta,
                          "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
                          "<EnqueueAsNext>0</EnqueueAsNext>");
    if (args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = soap(room->ip, "AVTransport", "RemoveAllTracksFromQueue", INSTANCE);
    if (err == ESP_OK) {
        err = soap(room->ip, "AVTransport", "AddURIToQueue", args);
    }
    free(args);
    if (err != ESP_OK) {
        return err;
    }
    char queue[192];
    snprintf(queue, sizeof(queue), INSTANCE "<CurrentURI>x-rincon-queue:%s#0</CurrentURI><CurrentURIMetaData>"
                                   "</CurrentURIMetaData>", room->uuid);
    if ((err = soap(room->ip, "AVTransport", "SetAVTransportURI", queue)) != ESP_OK) {
        return err;
    }
    return soap(room->ip, "AVTransport", "Play", INSTANCE "<Speed>1</Speed>");
}

esp_err_t sonos_play_favorite(const sonos_room_t *room, const sonos_fav_t *fav)
{
    ESP_LOGI(TAG, "Playing favourite %s in %s", fav->title, room->name);
    if (strncmp(fav->uri, "x-rincon-cpcontainer:", 21) == 0) {
        return play_queued(room, fav->uri, fav->meta); /* a playlist or album */
    }
    char *args = uri_args(INSTANCE, "CurrentURI", fav->uri, "CurrentURIMetaData", fav->meta, "");
    if (args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = soap(room->ip, "AVTransport", "SetAVTransportURI", args);
    free(args);
    if (err == ESP_OK) {
        err = soap(room->ip, "AVTransport", "Play", INSTANCE "<Speed>1</Speed>");
    }
    return err;
}

esp_err_t sonos_play_spotify(const sonos_room_t *room, const char *spotify_uri)
{
    ESP_LOGI(TAG, "Playing %s in %s", spotify_uri, room->name);
    char uri[160];
    char *meta = malloc(1024);
    if (meta == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* The one that worked before first. */
    int order[2] = { SPOTIFY_SERVICES[0], SPOTIFY_SERVICES[1] };
    if (s_spotify_service == SPOTIFY_SERVICES[1]) {
        order[0] = SPOTIFY_SERVICES[1];
        order[1] = SPOTIFY_SERVICES[0];
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (int i = 0; i < 2; i++) {
        const int svc = order[i];
        if (!sonos_spotify_share(spotify_uri, svc, uri, sizeof(uri), meta, 1024)) {
            break;
        }
        err = play_queued(room, uri, meta);
        if (err == ESP_OK) {
            s_spotify_service = svc;
            break;
        }
        if (s_fault == 0) {
            break; /* not refused: the network */
        }
        err = SONOS_ERR_NO_SPOTIFY;
    }
    free(meta);
    return err;
}

/* --------------------------------------------------------------------------
 * Covers
 * ------------------------------------------------------------------------ */

static bool image_chunk(const char *data, size_t len, void *ctx)
{
    return http_buf_append(ctx, data, len, TAG) == ESP_OK;
}

esp_err_t sonos_get_image(const char *url, unsigned char **data, size_t *len)
{
    http_buf_t b = { .max = SO_IMAGE_MAX };
    const bool https = strncmp(url, "https://", 8) == 0;
    const esp_err_t err =
        http_get_stream(url, yr_client_user_agent(), SO_TIMEOUT_MS, SO_IMAGE_MAX, https, image_chunk, &b, TAG);
    if (err != ESP_OK || b.buf == NULL) {
        http_buf_free(&b);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    *data = (unsigned char *)b.buf;
    *len = b.len;
    return ESP_OK;
}

void sonos_client_release(void)
{
    http_buf_free(&s_resp);
}
