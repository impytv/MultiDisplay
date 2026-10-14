/* Police, rescue and ambulance aircraft.
 *
 * Some services fly under their own callsigns: the Air Force's rescue
 * helicopters (330 squadron, military serials outside the civil register)
 * as "SAVER41", Norsk Luftambulanse's helicopters as "DOC02", Babcock's
 * ambulance planes as "BNO92J" and Swedish ones as "DFL..".
 *
 * The rest are found by owner and operator in the Civil Aviation
 * Authority's register (Luftfartstilsynet, Norges luftfartøyregister): one
 * web page per registration, .../registrerte-luftfartoy/?mark=LN-ORA, with
 * the entry as HTML-escaped JSON in a data attribute - "operator":null,
 * "mainOwner":"STATEN v/ POLITIDIREKTORATET". The page is ~46 KB, so it is
 * scanned as it arrives instead of being kept. Each registration is looked
 * up once: every answer - an owner, "not in the register" (404) or a page
 * without the entry - is kept while the display runs (the oldest give way
 * when the cache is full). Only a lookup that got no answer at all is tried
 * again. */

#include "esp_attr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "adsb_client.h"

static const char *TAG = "adsb_owners";

#define OWNER_CACHE      256 /* 6 KB of PSRAM */
#define OWNER_TIMEOUT_MS 10000
#define OWNER_URL        "https://www.luftfartstilsynet.no/aktorer/norges-luftfartoyregister/registrerte-luftfartoy/?mark="

typedef struct {
    char reg[9];
    adsb_role_t role;
    int64_t at_us; /* when looked up; 0 = free slot */
} owner_entry_t;

/* The register entry's fields the role comes from. */
enum { FIELD_OPERATOR, FIELD_OWNER, FIELDS };
static const char *const FIELD_KEY[FIELDS] = { "operator&quot;:", "mainOwner&quot;:" };
#define QUOT "&quot;"

/* Finds each FIELD_KEY in a page fed in pieces and keeps its value: the
 * text of a string ("&quot;...&quot;", up to the next '&'), or "" for
 * null. A mismatch starts the key over (a near miss that overlaps the real
 * key, which the page has no reason to contain, would hide it). */
typedef struct {
    size_t matched; /* letters of the key, then of QUOT, matched */
    enum { SEEK, OPEN, VALUE, DONE } state;
    char value[48];
    size_t len;
} field_scan_t;

typedef struct {
    field_scan_t f[FIELDS];
} owner_scan_t;

static EXT_RAM_BSS_ATTR owner_entry_t s_cache[OWNER_CACHE];
static esp_http_client_handle_t s_client;
static EXT_RAM_BSS_ATTR owner_scan_t s_scan;

static void scan_feed(owner_scan_t *s, const char *data, size_t n)
{
    for (int k = 0; k < FIELDS; k++) {
        const char *key = FIELD_KEY[k];
        const size_t key_len = strlen(key);
        field_scan_t *f = &s->f[k];
        for (size_t i = 0; i < n && f->state != DONE; i++) {
            const char c = data[i];
            switch (f->state) {
            case SEEK:
                if (c == key[f->matched]) {
                    if (++f->matched == key_len) {
                        f->state = OPEN;
                        f->matched = 0;
                    }
                } else {
                    f->matched = (c == key[0]) ? 1 : 0;
                }
                break;
            case OPEN: /* a string's opening &quot;, or null */
                if (c == QUOT[f->matched]) {
                    if (++f->matched == sizeof(QUOT) - 1) {
                        f->state = VALUE;
                    }
                } else {
                    f->state = DONE;
                }
                break;
            case VALUE:
                if (c == '&') {
                    f->state = DONE;
                } else if (f->len < sizeof(f->value) - 1) {
                    f->value[f->len++] = c;
                    f->value[f->len] = '\0';
                }
                break;
            case DONE:
                break;
            }
        }
    }
}

static bool scan_done(const owner_scan_t *s)
{
    for (int k = 0; k < FIELDS; k++) {
        if (s->f[k].state != DONE) {
            return false;
        }
    }
    return true;
}

static bool has_word(const char *text, const char *word)
{
    const size_t n = strlen(word);
    for (; *text; text++) {
        if (strncasecmp(text, word, n) == 0) {
            return true;
        }
    }
    return false;
}

/* The role an owner or operator's name gives. */
static adsb_role_t role_of_name(const char *name)
{
    if (has_word(name, "POLITIDIREKTORATET")) {
        return ADSB_ROLE_POLICE;
    }
    if (has_word(name, "luftambulanse") || has_word(name, "air ambulance") || has_word(name, "airambulance")) {
        return ADSB_ROLE_AMBULANCE;
    }
    if (has_word(name, "redningstjeneste") || has_word(name, "hovedredningssentral")) {
        return ADSB_ROLE_RESCUE;
    }
    return ADSB_ROLE_NONE;
}

/* The role a callsign gives: a service's prefix followed by a digit. */
static adsb_role_t role_of_callsign(const char *cs)
{
    static const struct {
        const char *prefix;
        adsb_role_t role;
    } PREFIXES[] = {
        { "SAVER", ADSB_ROLE_RESCUE },
        { "DOC", ADSB_ROLE_AMBULANCE },
        { "BNO", ADSB_ROLE_AMBULANCE },
        { "DFL", ADSB_ROLE_AMBULANCE },
    };
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        const size_t n = strlen(PREFIXES[i].prefix);
        if (strncmp(cs, PREFIXES[i].prefix, n) == 0 && isdigit((unsigned char)cs[n])) {
            return PREFIXES[i].role;
        }
    }
    return ADSB_ROLE_NONE;
}

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    /* Only a registered aircraft's page: an unknown one's 404 is an error page. */
    if (evt->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(evt->client) == 200) {
        scan_feed(&s_scan, evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

/* Norwegian registrations only: "LN-ORA". */
static bool is_norwegian(const char *reg)
{
    return strncmp(reg, "LN-", 3) == 0 && reg[3] != '\0';
}

static owner_entry_t *find(const char *reg)
{
    for (int i = 0; i < OWNER_CACHE; i++) {
        if (s_cache[i].at_us != 0 && strcmp(s_cache[i].reg, reg) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

/* The oldest slot (or a free one). */
static owner_entry_t *slot(void)
{
    owner_entry_t *e = &s_cache[0];
    for (int i = 1; i < OWNER_CACHE && e->at_us != 0; i++) {
        if (s_cache[i].at_us < e->at_us) {
            e = &s_cache[i];
        }
    }
    return e;
}

/* Look `reg` up: ESP_OK with its role in `role`, ESP_ERR_INVALID_RESPONSE
 * (and no role) for an answer without the entry, or the error of a lookup
 * that got no answer. */
static esp_err_t lookup(const char *reg, adsb_role_t *role)
{
    char url[sizeof(OWNER_URL) + 12];
    snprintf(url, sizeof(url), OWNER_URL "%s", reg);
    if (s_client == NULL) {
        const esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = on_event,
            .timeout_ms = OWNER_TIMEOUT_MS,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return ESP_FAIL;
        }
        esp_http_client_set_header(s_client, "User-Agent", CONFIG_MULTIDISPLAY_USER_AGENT);
    } else {
        esp_http_client_set_url(s_client, url);
    }
    memset(&s_scan, 0, sizeof(s_scan));
    esp_err_t err = esp_http_client_perform(s_client);
    const int status = esp_http_client_get_status_code(s_client);
    if (err != ESP_OK) {
        adsb_owners_close(); /* a fresh connection next time */
        return err;
    }
    *role = ADSB_ROLE_NONE;
    if (status == 404) {
        return ESP_OK; /* not in the register */
    }
    if (status != 200 || !scan_done(&s_scan)) {
        return ESP_ERR_INVALID_RESPONSE; /* the page has changed? */
    }
    *role = role_of_name(s_scan.f[FIELD_OPERATOR].value);
    if (*role == ADSB_ROLE_NONE) {
        *role = role_of_name(s_scan.f[FIELD_OWNER].value);
    }
    ESP_LOGD(TAG, "%s: %s / %s", reg, s_scan.f[FIELD_OPERATOR].value, s_scan.f[FIELD_OWNER].value);
    return ESP_OK;
}

esp_err_t adsb_owners_fill(adsb_result_t *res, int max_fetches)
{
    const int64_t now = esp_timer_get_time();
    esp_err_t result = ESP_OK;
    for (int i = 0; i < res->count; i++) {
        adsb_aircraft_t *a = &res->ac[i];
        a->role = role_of_callsign(a->callsign);
        if (a->role != ADSB_ROLE_NONE || !is_norwegian(a->reg)) {
            continue;
        }
        const owner_entry_t *hit = find(a->reg);
        if (hit == NULL && max_fetches > 0) {
            max_fetches--;
            adsb_role_t role;
            esp_err_t err = lookup(a->reg, &role);
            if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE) { /* an answer: not asked again */
                owner_entry_t *e = slot();
                snprintf(e->reg, sizeof(e->reg), "%s", a->reg);
                e->role = role;
                e->at_us = now;
                hit = e;
            }
            if (err != ESP_OK) {
                result = err;
                max_fetches = 0; /* the site is having trouble: no more this poll */
            }
        }
        if (hit != NULL) {
            a->role = hit->role;
        }
    }
    return result;
}

void adsb_owners_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}
