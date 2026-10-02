/* Settings backup (app_config_to_json / app_config_from_json): the setup
 * page's "Sikkerhetskopi". A versioned JSON file, readable and editable by
 * hand; the WiFi network and the secrets are never in it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "app_config.h"

#define BACKUP_FORMAT  "multidisplay-innstillinger"
#define BACKUP_VERSION 1

static void add_hhmm(cJSON *o, const char *key, uint16_t min)
{
    char t[8];
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(min / 60 % 24), (unsigned)(min % 60));
    cJSON_AddStringToObject(o, key, t);
}

char *app_config_to_json(const app_config_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "format", BACKUP_FORMAT);
    cJSON_AddNumberToObject(root, "version", BACKUP_VERSION);
    cJSON_AddNumberToObject(root, "theme", c->theme);

    cJSON *night = cJSON_AddObjectToObject(root, "night");
    cJSON_AddBoolToObject(night, "enabled", c->dim_enabled);
    add_hhmm(night, "from", c->dim_start);
    add_hhmm(night, "to", c->dim_end);
    cJSON_AddBoolToObject(night, "screen_off", c->night_off);

    cJSON *fonts = cJSON_AddObjectToObject(root, "fonts");
    cJSON_AddNumberToObject(fonts, "title_px", c->title_px);
    cJSON_AddBoolToObject(fonts, "title_bold", c->title_bold);
    cJSON_AddNumberToObject(fonts, "text_px", c->text_px);
    cJSON_AddBoolToObject(fonts, "text_bold", c->text_bold);

    cJSON *rot = cJSON_AddObjectToObject(root, "rotation");
    cJSON_AddNumberToObject(rot, "idle_min", c->auto_idle_min);
    cJSON_AddNumberToObject(rot, "per_screen_s", c->auto_dwell_s);
    cJSON_AddBoolToObject(rot, "overview", c->auto_overview);
    cJSON_AddBoolToObject(rot, "pause_at_night", c->auto_night_pause);

    cJSON_AddStringToObject(root, "yr_email", c->yr_email);
    cJSON_AddStringToObject(root, "barentswatch_client_id", c->ais_client_id);

    cJSON *upd = cJSON_AddObjectToObject(root, "updates");
    cJSON_AddBoolToObject(upd, "automatic", c->ota_auto);
    cJSON_AddStringToObject(upd, "url", c->ota_url);

    /* The calendar's addresses are secrets, so not here. */
    cJSON *cal = cJSON_AddObjectToObject(root, "calendar");
    cJSON_AddBoolToObject(cal, "show", c->cal_show);
    cJSON_AddBoolToObject(cal, "rotate", c->cal_rotate);

    cJSON *locs = cJSON_AddArrayToObject(root, "locations");
    for (int i = 0; i < c->location_count && i < APP_CONFIG_MAX_LOCATIONS; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "name", c->locations[i].name);
        cJSON_AddStringToObject(l, "lat", c->locations[i].lat);
        cJSON_AddStringToObject(l, "lon", c->locations[i].lon);
        cJSON_AddNumberToObject(l, "show", c->show[i]);
        cJSON_AddNumberToObject(l, "rotate", c->auto_show[i]);
        cJSON_AddNumberToObject(l, "aircraft_km", c->radar_km[i]);
        cJSON_AddNumberToObject(l, "ship_km", c->ship_km[i]);
        cJSON_AddNumberToObject(l, "ship_min_m", c->ship_min_len_m[i]);
        cJSON_AddNumberToObject(l, "ship_inner_km", c->ship_near_km[i]);
        cJSON_AddNumberToObject(l, "ship_inner_min_m", c->ship_near_min_len_m[i]);
        cJSON_AddNumberToObject(l, "rain_km", c->rain_km[i]);
        cJSON_AddStringToObject(l, "departures", c->departures[i]);
        cJSON_AddItemToArray(locs, l);
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

/* Setters that leave the field alone when the key is missing or of the
 * wrong type. */
static void get_u8(const cJSON *o, const char *k, uint8_t *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 255) {
        *dst = (uint8_t)v->valuedouble;
    }
}

static void get_u16(const cJSON *o, const char *k, uint16_t *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 65535) {
        *dst = (uint16_t)v->valuedouble;
    }
}

static void get_bool(const cJSON *o, const char *k, uint8_t *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) {
        *dst = cJSON_IsTrue(v) ? 1 : 0;
    }
}

/* A string that must fit; false if present but too long. */
static bool get_str(const cJSON *o, const char *k, char *dst, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v)) {
        return true;
    }
    if (strlen(v->valuestring) >= len) {
        return false;
    }
    snprintf(dst, len, "%s", v->valuestring);
    return true;
}

static void get_hhmm(const cJSON *o, const char *k, uint16_t *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    int h, m;
    char extra;
    if (cJSON_IsString(v) && sscanf(v->valuestring, "%d:%d%c", &h, &m, &extra) == 2 && h >= 0 && h < 24 &&
        m >= 0 && m < 60) {
        *dst = (uint16_t)(h * 60 + m);
    }
}

bool app_config_from_json(const char *json, app_config_t *cfg, char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (root == NULL || !cJSON_IsString(fmt) || strcmp(fmt->valuestring, BACKUP_FORMAT) != 0) {
        snprintf(err, err_len, "Filen er ikke en sikkerhetskopi av innstillingene.");
        cJSON_Delete(root);
        return false;
    }
    if (!cJSON_IsNumber(ver) || ver->valuedouble > BACKUP_VERSION) {
        snprintf(err, err_len, "Sikkerhetskopien er fra en nyere programvare.");
        cJSON_Delete(root);
        return false;
    }

    /* Work on a copy: nothing changes unless all of it is good. */
    app_config_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        snprintf(err, err_len, "Ikke nok minne.");
        cJSON_Delete(root);
        return false;
    }
    *c = *cfg;
    bool ok = true;

    get_u8(root, "theme", &c->theme);
    const cJSON *night = cJSON_GetObjectItemCaseSensitive(root, "night");
    get_bool(night, "enabled", &c->dim_enabled);
    get_hhmm(night, "from", &c->dim_start);
    get_hhmm(night, "to", &c->dim_end);
    get_bool(night, "screen_off", &c->night_off);
    const cJSON *fonts = cJSON_GetObjectItemCaseSensitive(root, "fonts");
    get_u8(fonts, "title_px", &c->title_px);
    get_bool(fonts, "title_bold", &c->title_bold);
    get_u8(fonts, "text_px", &c->text_px);
    get_bool(fonts, "text_bold", &c->text_bold);
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    get_u16(rot, "idle_min", &c->auto_idle_min);
    get_u16(rot, "per_screen_s", &c->auto_dwell_s);
    get_bool(rot, "overview", &c->auto_overview);
    get_bool(rot, "pause_at_night", &c->auto_night_pause);
    ok &= get_str(root, "yr_email", c->yr_email, sizeof(c->yr_email));
    if (!app_config_email_valid(c->yr_email)) {
        c->yr_email[0] = '\0';
    }
    char old_id[sizeof(c->ais_client_id)];
    snprintf(old_id, sizeof(old_id), "%s", c->ais_client_id);
    ok &= get_str(root, "barentswatch_client_id", c->ais_client_id, sizeof(c->ais_client_id));
    if (strcmp(old_id, c->ais_client_id) != 0) {
        c->ais_client_secret[0] = '\0'; /* the secret went with the old client */
    }
    const cJSON *upd = cJSON_GetObjectItemCaseSensitive(root, "updates");
    get_bool(upd, "automatic", &c->ota_auto);
    ok &= get_str(upd, "url", c->ota_url, sizeof(c->ota_url));
    if (c->ota_url[0] != '\0' && strncmp(c->ota_url, "http://", 7) != 0 && strncmp(c->ota_url, "https://", 8) != 0) {
        snprintf(c->ota_url, sizeof(c->ota_url), "%s", cfg->ota_url);
    }
    const cJSON *cal = cJSON_GetObjectItemCaseSensitive(root, "calendar");
    get_bool(cal, "show", &c->cal_show);
    get_bool(cal, "rotate", &c->cal_rotate);
    if (!ok) {
        snprintf(err, err_len, "En tekst i sikkerhetskopien er for lang.");
    }

    const cJSON *locs = cJSON_GetObjectItemCaseSensitive(root, "locations");
    if (ok && locs != NULL) {
        const int n = cJSON_GetArraySize(locs);
        if (!cJSON_IsArray(locs) || n < 1 || n > APP_CONFIG_MAX_LOCATIONS) {
            snprintf(err, err_len, "Sikkerhetskopien m\xC3\xA5 ha fra 1 til %d steder.", APP_CONFIG_MAX_LOCATIONS);
            ok = false;
        }
        for (int i = 0; ok && i < n; i++) {
            const cJSON *l = cJSON_GetArrayItem(locs, i);
            app_location_t loc = { 0 };
            ok = get_str(l, "name", loc.name, sizeof(loc.name)) && get_str(l, "lat", loc.lat, sizeof(loc.lat)) &&
                 get_str(l, "lon", loc.lon, sizeof(loc.lon));
            if (!ok || !app_config_coord_valid(loc.lat, true) || !app_config_coord_valid(loc.lon, false)) {
                snprintf(err, err_len, "Sted %d har ugyldig navn, breddegrad eller lengdegrad.", i + 1);
                ok = false;
                break;
            }
            c->locations[i] = loc;
            c->show[i] = APP_SHOW_WEATHER;
            c->auto_show[i] = 0;
            c->radar_km[i] = APP_CONFIG_RADAR_KM_DEFAULT;
            c->ship_km[i] = APP_CONFIG_SHIP_KM_DEFAULT;
            c->rain_km[i] = APP_CONFIG_RAIN_KM_DEFAULT;
            c->ship_min_len_m[i] = c->ship_near_km[i] = c->ship_near_min_len_m[i] = 0;
            c->departures[i][0] = '\0';
            get_u8(l, "show", &c->show[i]);
            get_u8(l, "rotate", &c->auto_show[i]);
            get_u16(l, "aircraft_km", &c->radar_km[i]);
            get_u16(l, "ship_km", &c->ship_km[i]);
            get_u16(l, "ship_min_m", &c->ship_min_len_m[i]);
            get_u16(l, "ship_inner_km", &c->ship_near_km[i]);
            get_u16(l, "ship_inner_min_m", &c->ship_near_min_len_m[i]);
            get_u16(l, "rain_km", &c->rain_km[i]);
            if (!get_str(l, "departures", c->departures[i], sizeof(c->departures[i]))) {
                snprintf(err, err_len, "Avgangene for sted %d er for lange.", i + 1);
                ok = false;
            }
        }
        if (ok) {
            c->location_count = (uint8_t)n;
        }
    }
    cJSON_Delete(root);
    if (ok) {
        app_config_sanitize(c);
        *cfg = *c;
    }
    free(c);
    return ok;
}
