#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "wifi_provision.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "ota_writer.h"
#include "form_util.h"
#include "cJSON.h"
#include "mdns.h"
#include "restart.h"

static const char *TAG = "wifi_provision";

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define STA_CONNECT_TIMEOUT_MS  45000
#define PORTAL_AP_IP            "192.168.4.1"
#define PORTAL_MAX_SCAN         20

#define BIT_CONNECTED  BIT0
#define BIT_GAVE_UP    BIT1

/* Reconnect after a drop: first after RECONNECT_FIRST_MS, doubling up to
 * RECONNECT_MAX_MS (see wifi_event_handler). */
#define RECONNECT_FIRST_MS      300
#define RECONNECT_MAX_MS        10000

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_reconnect_timer;
static int s_retries;
static bool s_stop_reconnect;
static httpd_handle_t s_httpd;
static wifi_provision_status_fn s_status;
static char s_ap_ssid[24];
static char s_sta_ip[16]; /* "" until the first IP_EVENT_STA_GOT_IP */
static volatile bool s_sta_up; /* has an IP right now */
/* The setup page's password ("" = none), and whether BOOT was held at
 * power-on, which opens the page without it - the way back in when the
 * password is forgotten. Set by wifi_provision_connect. */
static char s_web_pass[APP_CONFIG_PASS_MAX];
static char s_hostname[APP_CONFIG_DEVNAME_MAX] = APP_CONFIG_DEVNAME_DEFAULT; /* <name>.local */
static esp_netif_t *s_sta_netif;
static bool s_auth_bypass;

static void status(const char *msg)
{
    if (s_status) {
        s_status(msg);
    }
}

/* --------------------------------------------------------------------------
 * Small text helpers
 * ------------------------------------------------------------------------ */

/* Append `src` to `dst` (bounded by dst_end), HTML-escaping " & < >. */
static char *html_escape_append(char *dst, char *dst_end, const char *src)
{
    for (; *src && dst < dst_end - 6; src++) {
        switch (*src) {
        case '"': dst += sprintf(dst, "&quot;"); break;
        case '&': dst += sprintf(dst, "&amp;"); break;
        case '<': dst += sprintf(dst, "&lt;"); break;
        case '>': dst += sprintf(dst, "&gt;"); break;
        default:  *dst++ = *src; break;
        }
    }
    *dst = '\0';
    return dst;
}

/* --------------------------------------------------------------------------
 * Config web page
 * ------------------------------------------------------------------------ */

/* The page's look and behaviour are files of their own (setup_page.css and
 * setup_page.js, embedded and served as /setup.css and /setup.js). */
static const char PAGE_HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<html lang=no><link rel=stylesheet href=/setup.css>";

static const char PAGE_LOC_INTRO[] =
    "<h2>Steder</h2><small>Skjermen viser ett sted om gangen; trykk p\xC3\xA5 h\xC3\xB8yre halvdel for neste, venst"
    "re for forrige. \xE2\x96\xB2/\xE2\x96\xBC flytter et sted. S\xC3\xB8kene etter steder og holdeplasser trenger "
    "internett, s\xC3\xA5 de virker ikke p\xC3\xA5 skjermens eget oppsettnett.</small>";

/* Maintenance (upload, backup, health), the save bar and the end of the form. */
static const char PAGE_MAINT[] =
    "<fieldset><legend>Last opp programvare</legend><small>build/multi_display.bin fra prosjektet, signert med pros"
    "jektets n\xC3\xB8kkel. Skjermen starter p\xC3\xA5 nytt med den nye programvaren, og g\xC3\xA5r tilbake til den"
    " forrige om den nye ikke starter som den skal. Eldre versjoner godtas ogs\xC3\xA5.</small><input type=file id="
    "fw accept=.bin style='margin-top:.6rem'><button type=button id=fwb class=lt2>Last opp og start p\xC3\xA5 nytt<"
    "/button><small id=fws></small></fieldset><fieldset><legend>Sikkerhetskopi</legend><small>Alle innstillingene u"
    "nntatt navn, WiFi og passord/hemmeligheter, som en fil. Gjenoppretting beholder skjermens navn, WiFi og passor"
    "d, og starter den p\xC3\xA5 nytt.</small><p><a href=/config.json download>Last ned innstillingene</a></p><inpu"
    "t type=file id=cfgf accept=.json><button type=button id=cfgb class=lt2>Gjenopprett</button><small id=cfgs></sm"
    "all></fieldset><fieldset><legend>Driftsstatus</legend><div id=diag><small>Henter...</small></div><p><small><a "
    "href=/log target=_blank>Logg</a> &middot; <a href=/screen.png target=_blank>Skjermbilde</a><span id=cdl hidden"
    "> &middot; <a href=/coredump>Krasjdump</a> &middot; <a href=# id=cde>slett den</a></span></small></p></fieldse"
    "t></details><div class=bar><span id=dirty></span><button type=submit>Lagre og start p\xC3\xA5 nytt</button></d"
    "iv></form>";

static const char PAGE_SCRIPTS[] = "<script src=/setup.js></script><script src=/dep.js></script>";

/* The screens a location can show, in their order: the "show" and "in the
 * rotation" checkbox names (with the location's index after them) and the
 * APP_SHOW_* bit. */
static const struct {
    const char *vis, *rot, *label;
    uint8_t bit;
} SCREENS[] = {
    { "wx", "aw", "V&aelig;r", APP_SHOW_WEATHER }, { "uk", "au", "Uke", APP_SHOW_WEEK },
    { "ac", "aa", "Fly", APP_SHOW_RADAR },         { "sh", "as", "Skip", APP_SHOW_SHIPS },
    { "rn", "ar", "Nedb&oslash;r", APP_SHOW_RAIN }, { "dp", "ad", "Avganger", APP_SHOW_DEPARTURES },
    { "lq", "al", "Luft", APP_SHOW_AIR },          { "td", "at", "Tidevann", APP_SHOW_TIDE },
};

/* A screen's own settings, right under its checkboxes; shown only while
 * the screen is ticked (data-need, see PAGE_SCRIPTS). */
static char *build_screen_settings(char *p, char *end, const app_config_t *cfg, int i, uint8_t bit)
{
    const bool filled = (i < cfg->location_count);
    switch (bit) {
    case APP_SHOW_RADAR:
        p += snprintf(p, end - p, "<div data-need=ac><label>Radius (km)</label>"
                      "<input name=radarkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>",
                      i, APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX,
                      filled ? cfg->radar_km[i] : APP_CONFIG_RADAR_KM_DEFAULT);
        break;
    case APP_SHOW_RAIN:
        p += snprintf(p, end - p, "<div data-need=rn><label>Radius (km)</label>"
                      "<input name=rainkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>",
                      i, APP_CONFIG_RAIN_KM_MIN, APP_CONFIG_RAIN_KM_MAX,
                      filled ? cfg->rain_km[i] : APP_CONFIG_RAIN_KM_DEFAULT);
        break;
    case APP_SHOW_SHIPS:
        p += snprintf(p, end - p,
                      "<div data-need=sh><div class=row><div><label>Radius (km)</label>"
                      "<input name=shipkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>"
                      "<div><label>Korteste skip (m)</label>"
                      "<input name=shipminlen%d type=number inputmode=numeric min=0 max=%d value=%d></div></div>"
                      "<div class=row><div><label>Indre sone (km)</label>"
                      "<input name=shipnearkm%d type=number inputmode=numeric min=0 max=%d value=%d></div>"
                      "<div><label>Korteste innenfor (m)</label>"
                      "<input name=shipnearlen%d type=number inputmode=numeric min=0 max=%d value=%d></div></div>"
                      "<small>Kortere skip, og skip som ikke oppgir lengde, vises ikke (0 viser alle). "
                      "Innenfor den indre sonen (0 = ingen) gjelder en egen korteste lengde.</small></div>",
                      i, APP_CONFIG_SHIP_KM_MIN, APP_CONFIG_SHIP_KM_MAX,
                      filled ? cfg->ship_km[i] : APP_CONFIG_SHIP_KM_DEFAULT,
                      i, APP_CONFIG_SHIP_MIN_LEN_MAX, filled ? cfg->ship_min_len_m[i] : 0,
                      i, APP_CONFIG_SHIP_NEAR_KM_MAX, filled ? cfg->ship_near_km[i] : 0,
                      i, APP_CONFIG_SHIP_MIN_LEN_MAX, filled ? cfg->ship_near_min_len_m[i] : 0);
        break;
    case APP_SHOW_DEPARTURES:
        p += snprintf(p, end - p, "<div data-need=dp><input name=dep%d type=hidden data-max=%d value=\"",
                      i, APP_CONFIG_DEPARTURES_MAX - 1);
        if (filled) {
            p = html_escape_append(p, end, cfg->departures[i]);
        }
        p += snprintf(p, end - p, "\"></div>");
        break;
    default:
        break;
    }
    return p;
}

/* One location's block: collapsed, hidden when not in use (the page's "+ Legg
 * til sted" shows it). Each screen's settings follow its checkboxes. */
static char *build_location(char *p, char *end, const app_config_t *cfg, int i)
{
    const bool filled = (i < cfg->location_count);
    p += snprintf(p, end - p,
                  "<details class=loc%s><summary><span class=lt>Sted %d</span>"
                  "<button type=button class=mv data-d=-1 aria-label='Flytt opp'>&#9650;</button>"
                  "<button type=button class=mv data-d=1 aria-label='Flytt ned'>&#9660;</button>"
                  "</summary><fieldset data-loc=%d>"
                  "<label>Finn sted</label><input class=plq type=search autocomplete=off "
                  "placeholder='S&oslash;k etter sted, f.eks. Nittedal'><div class=hits></div>"
                  "<label>Navn</label><input name=name%d value=\"",
                  filled ? "" : " hidden", i + 1, i, i);
    if (filled) {
        p = html_escape_append(p, end, cfg->locations[i].name);
    }
    p += snprintf(p, end - p, "\"><div class=row><div><label>Breddegrad</label>"
                  "<input name=lat%d inputmode=decimal value=\"", i);
    if (filled) {
        p = html_escape_append(p, end, cfg->locations[i].lat);
    }
    p += snprintf(p, end - p, "\"></div><div><label>Lengdegrad</label>"
                  "<input name=lon%d inputmode=decimal value=\"", i);
    if (filled) {
        p = html_escape_append(p, end, cfg->locations[i].lon);
    }
    p += snprintf(p, end - p, "\"></div></div><label>Skjermer</label><div class=scr>");

    const int show = filled ? cfg->show[i] : APP_SHOW_WEATHER;
    const int auto_show = filled ? cfg->auto_show[i] : 0;
    for (size_t k = 0; k < sizeof(SCREENS) / sizeof(SCREENS[0]); k++) {
        p += snprintf(p, end - p,
                      "<div class=sg><div class=sr><label><input type=checkbox class=vis name=%s%d value=1%s>%s</label>"
                      "<label class=rot><input type=checkbox name=%s%d value=1%s>i automatisk bytte</label></div>",
                      SCREENS[k].vis, i, (show & SCREENS[k].bit) ? " checked" : "", SCREENS[k].label,
                      SCREENS[k].rot, i, (auto_show & SCREENS[k].bit) ? " checked" : "");
        p = build_screen_settings(p, end, cfg, i, SCREENS[k].bit);
        p += snprintf(p, end - p, "</div>");
    }
    p += snprintf(p, end - p, "</div><button type=button class='rm lt2'>Fjern stedet</button>"
                  "</fieldset></details>");
    return p;
}

/* The calendar screen: one for the display. The addresses are secrets, so
 * never sent back: a blank field keeps the saved one (see save_form_into). */
static char *build_calendar(char *p, char *end, const app_config_t *cfg)
{
    static const char *const COLOUR[APP_CONFIG_CAL_FEEDS] = { "#2e86de", "#e67e22", "#27ae60" };
    p += snprintf(p, end - p,
                  "<h2>Kalender</h2><div class=chk><label><input type=checkbox name=calshow value=1%s>"
                  "Vis kalenderen</label><label><input type=checkbox name=calrot value=1%s>"
                  "i automatisk bytte</label></div>"
                  "<small>De neste to ukene fra opptil tre kalendere, hver i sin farge, som en egen "
                  "skjerm etter oversikten. Bruk kalenderens hemmelige iCal-adresse: i Google Kalender "
                  "under innstillingene for kalenderen, <i>Hemmelig adresse i iCal-format</i>; i Outlook "
                  "under <i>Delte kalendere</i>, <i>Publiser en kalender</i> (ICS-lenken).</small>",
                  cfg->cal_show ? " checked" : "", cfg->cal_rotate ? " checked" : "");
    for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
        const bool set = cfg->cal_url[i][0] != '\0';
        p += snprintf(p, end - p,
                      "<label><span style='display:inline-block;width:.7em;height:.7em;border-radius:50%%;"
                      "background:%s;margin-right:.4em'></span>Kalender %d</label>"
                      "<input name=calurl%d autocomplete=off inputmode=url maxlength=%d placeholder=\"%s\">",
                      COLOUR[i], i + 1, i, APP_CONFIG_CAL_URL_MAX - 1,
                      set ? "Lagret - la st&aring; tomt for &aring; beholde" : "https://...ics");
        if (set) {
            p += snprintf(p, end - p, "<div class=chk><label><input type=checkbox name=caloff%d value=1>"
                                      "Fjern kalenderen</label></div>", i);
        }
    }
    return p;
}

/* Build the full page into a heap buffer (caller frees). The locations come
 * first; everything else is in collapsed sections. */
static char *build_page(const app_config_t *cfg)
{
    const size_t cap = 36864;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    char *p = buf;
    char *end = buf + cap;

    p += snprintf(p, end - p, "%s<title>", PAGE_HEAD);
    p = html_escape_append(p, end, cfg->device_name);
    p += snprintf(p, end - p, " - MultiDisplay</title><h1>MultiDisplay oppsett: ");
    p = html_escape_append(p, end, cfg->device_name);
    p += snprintf(p, end - p, "</h1><form method=post action=/save id=cf novalidate>"
                  "<label>Navn p&aring; skjermen</label><input name=devname autocomplete=off maxlength=%d "
                  "value=\"", APP_CONFIG_DEVNAME_MAX - 1);
    p = html_escape_append(p, end, cfg->device_name);
    p += snprintf(p, end - p, "\"><small>Skjermen finnes p&aring; <b>http://");
    p = html_escape_append(p, end, cfg->device_name);
    p += snprintf(p, end - p, ".local/</b> i nettverket. Gi hver skjerm sitt eget navn; bare a-z, "
                  "0-9 og bindestrek (&aelig;, &oslash;, &aring; blir ae, o, aa).</small>");

    /* Locations. */
    p += snprintf(p, end - p, "%s", PAGE_LOC_INTRO);
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        p = build_location(p, end, cfg, i);
    }
    p += snprintf(p, end - p, "<button type=button id=addloc class=lt2>+ Legg til sted</button>");
    p += snprintf(p, end - p,
                  "<h2>Oversikt</h2><div class=chk><label><input type=checkbox name=ovshow value=1%s>"
                  "Vis oversikten</label><label><input type=checkbox name=autoov value=1%s>"
                  "i automatisk bytte</label></div>"
                  "<small>Tabellen med v&aelig;ret for alle stedene, f&oslash;rst av skjermene. Uten den "
                  "begynner skjermene p&aring; kalenderen eller det f&oslash;rste stedet.</small>",
                  cfg->ov_show ? " checked" : "", cfg->auto_overview ? " checked" : "");
    p = build_calendar(p, end, cfg);
    p += snprintf(p, end - p, "<h2>Innstillinger</h2>");

    /* Look: theme, night, text sizes. Times as plain 24-hour text - a time
     * input follows the browser's language and may show AM/PM. */
    p += snprintf(p, end - p,
                  "<details class=sec><summary>Visning</summary>"
                  "<label>Tema</label><select name=theme>"
                  "<option value=0%s>Lyst</option><option value=1%s>M&oslash;rkt</option></select>"
                  "<label>Natt</label><div class=chk>"
                  "<label><input type=checkbox name=dimon value=1%s>Nattmodus</label></div>"
                  "<div class=row><div><label>Fra</label>"
                  "<input name=dimstart inputmode=numeric maxlength=5 placeholder=22:00 value=%02u:%02u></div>"
                  "<div><label>Til</label>"
                  "<input name=dimend inputmode=numeric maxlength=5 placeholder=07:00 value=%02u:%02u></div></div>"
                  "<select name=nightoff style='margin-top:.5rem'>"
                  "<option value=0%s>Demp skjermen</option><option value=1%s>Sl&aring; av skjermen</option></select>"
                  "<small>Lokal tid, 24 timer. Dempet er skjermen fortsatt lesbar, bare m&oslash;rkere. "
                  "Avsl&aring;tt lyser den i ett minutt n&aring;r du tar p&aring; den.</small>",
                  cfg->theme == APP_THEME_DARK ? "" : " selected", cfg->theme == APP_THEME_DARK ? " selected" : "",
                  cfg->dim_enabled ? " checked" : "",
                  cfg->dim_start / 60, cfg->dim_start % 60, cfg->dim_end / 60, cfg->dim_end % 60,
                  cfg->night_off ? "" : " selected", cfg->night_off ? " selected" : "");
    p += snprintf(p, end - p,
                  "<fieldset><legend>Skrift</legend>"
                  "<div class=row><div><label>Stedsnavn (px)</label>"
                  "<input name=titlepx type=number inputmode=numeric min=%d max=%d value=%u>"
                  "<div class=chk><label><input type=checkbox name=titlebold value=1%s>Fet</label></div></div>"
                  "<div><label>Annen tekst (px)</label>"
                  "<input name=textpx type=number inputmode=numeric min=%d max=%d value=%u>"
                  "<div class=chk><label><input type=checkbox name=textbold value=1%s>Fet</label></div></div></div>"
                  "<small>Stedsnavnet st&aring;r &oslash;verst p&aring; hver skjerm; annen tekst er "
                  "alt annet. Standard: %d og %d px, ikke fet.</small></fieldset></details>",
                  APP_CONFIG_TITLE_PX_MIN, APP_CONFIG_TITLE_PX_MAX, cfg->title_px, cfg->title_bold ? " checked" : "",
                  APP_CONFIG_TEXT_PX_MIN, APP_CONFIG_TEXT_PX_MAX, cfg->text_px, cfg->text_bold ? " checked" : "",
                  APP_CONFIG_TITLE_PX_DEFAULT, APP_CONFIG_TEXT_PX_DEFAULT);

    p += snprintf(p, end - p,
                  "<details class=sec><summary>Automatisk bytte</summary>"
                  "<div class=row><div><label>Etter (min uten trykk)</label>"
                  "<input name=autoidle type=number inputmode=numeric min=0 max=%d value=%u></div>"
                  "<div><label>Per skjerm (s)</label>"
                  "<input name=autodwell type=number inputmode=numeric min=%d max=%d value=%u></div></div>"
                  "<div class=chk><label><input type=checkbox name=autonight value=1%s>Stopp om natta</label></div>"
                  "<small>Etter s&aring; mange minutter uten trykk g&aring;r skjermen videre til neste "
                  "skjerm som er krysset av for <i>i automatisk bytte</i> (under stedene, oversikten og "
                  "kalenderen), og blir st&aring;ende s&aring; lenge p&aring; hver. Et trykk stopper "
                  "det til skjermen har v&aelig;rt i fred s&aring; lenge igjen. 0 minutter sl&aring;r det "
                  "av.</small></details>",
                  APP_CONFIG_AUTO_IDLE_MIN_MAX, cfg->auto_idle_min,
                  APP_CONFIG_AUTO_DWELL_S_MIN, APP_CONFIG_AUTO_DWELL_S_MAX, cfg->auto_dwell_s,
                  cfg->auto_night_pause ? " checked" : "");

    /* Access and passwords, collapsed - open in the setup portal, where
     * the WiFi is what's being set up. */
    p += snprintf(p, end - p, "<details class=sec%s><summary>Tilgang og passord</summary>",
                  s_sta_up ? "" : " open");
    p += snprintf(p, end - p, "<label>WiFi-nett</label>"
                  "<input name=ssid list=nets autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->wifi_ssid);
    p += snprintf(p, end - p, "\"><datalist id=nets></datalist>");

    /* Saved secrets are never sent back in the page: a blank field keeps
     * them (see save_form_into). */
    p += snprintf(p, end - p, "<label>WiFi-passord</label>"
                  "<input name=pass type=password autocomplete=new-password placeholder=\"%s\">"
                  "<small>%s</small>",
                  cfg->wifi_pass[0] ? "Lagret" : "",
                  cfg->wifi_pass[0] ? "La st&aring; tomt for &aring; beholde det lagrede. Et nytt nett uten "
                                      "passord: la st&aring; tomt."
                                    : "La st&aring; tomt for et &aring;pent nett");

    p += snprintf(p, end - p,
                  "<label>Kontakt-e-post for yr</label>"
                  "<input name=yremail type=email autocomplete=email value=\"");
    p = html_escape_append(p, end, cfg->yr_email);
    p += snprintf(p, end - p, "\"><small>Sendes til api.met.no i User-Agent-headeren, "
                  "slik vilk&aring;rene deres krever. La st&aring; tomt for &aring; bruke den "
                  "innebygde.</small>");

    p += snprintf(p, end - p,
                  "<fieldset><legend>BarentsWatch (skipstrafikk)</legend>"
                  "<small>API-klient fra barentswatch.no/minside, med tilgang "
                  "til AIS-API-et. Trengs bare for skipstrafikk.</small>"
                  "<label>Klient-ID</label><input name=aisid autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->ais_client_id);
    p += snprintf(p, end - p, "\"><label>Klienthemmelighet</label>"
                  "<input name=aissec type=password autocomplete=new-password placeholder=\"%s\">"
                  "%s</fieldset>",
                  cfg->ais_client_secret[0] ? "Lagret" : "",
                  cfg->ais_client_secret[0] ? "<small>La st&aring; tomt for &aring; beholde den lagrede.</small>" : "");

    p += snprintf(p, end - p,
                  "<fieldset><legend>Passord for oppsettsiden</legend>"
                  "<small>Valgfritt. N&aring;r det er satt, sp&oslash;r denne siden etter det (hvilket som helst "
                  "brukernavn). Det sendes ukryptert over WiFi, s&aring; ikke bruk et viktig passord. Glemt "
                  "det? Hold BOOT inne mens skjermen sl&aring;s p&aring;, s&aring; &aring;pner oppsettnettet uten "
                  "passord.</small>"
                  "<label>Nytt passord</label>"
                  "<input name=webpass type=password autocomplete=new-password placeholder=\"%s\">",
                  cfg->web_pass[0] ? "Lagret - la st&aring; tomt for &aring; beholde" : "Ingen");
    if (cfg->web_pass[0]) {
        p += snprintf(p, end - p, "<div class=chk><label><input type=checkbox name=webpassoff>"
                                  "Fjern passordet</label></div>");
    }
    p += snprintf(p, end - p, "</fieldset></details>");

    /* Firmware updates (main/updater.c): the settings are saved with the
     * form; the status line and the buttons talk to /ota/status, /ota/check
     * and /ota/install (see PAGE_SCRIPTS), which only exist once connected. */
    p += snprintf(p, end - p,
                  "<details class=sec><summary>Vedlikehold</summary>"
                  "<fieldset><legend>Programvareoppdatering</legend>"
                  "<div class=chk><label><input type=checkbox name=otaauto value=1%s>"
                  "Installer ny programvare automatisk</label></div>"
                  "<div class=row><div><label>Sjekk kl.</label>"
                  "<input name=otaat inputmode=numeric maxlength=5 placeholder=03:30 value=%02u:%02u></div>"
                  "<div><label>Og deretter hver (timer)</label>"
                  "<input name=otaevery type=number inputmode=numeric min=%d max=%d value=%u></div></div>"
                  "<small>Lokal tid, 24 timer. 24 timer sjekker &eacute;n gang i d&oslash;gnet; 6 timer fra "
                  "03:30 sjekker 03:30, 09:30, 15:30 og 21:30. Ny programvare installeres ved disse "
                  "sjekkene, n&aring;r det er krysset av over.</small>"
                  "<label>Oppdateringsadresse</label><input name=otaurl type=url autocomplete=off value=\"",
                  cfg->ota_auto ? " checked" : "", cfg->ota_at / 60, cfg->ota_at % 60, APP_CONFIG_OTA_EVERY_H_MIN,
                  APP_CONFIG_OTA_EVERY_H_MAX, cfg->ota_every_h);
    p = html_escape_append(p, end, cfg->ota_url);
    p += snprintf(p, end - p,
                  "\"><small>manifest.json p&aring; oppdateringssiden; manifest-test.json der for "
                  "testversjoner. Bare programvare signert med prosjektets n&oslash;kkel godtas.</small>"
                  "<p id=otast style='margin:.6rem 0 0'><small>Sjekker...</small></p><div id=otanew></div>"
                  "<div class=row><div><button type=button id=otachk class=lt2>Sjekk n&aring;</button></div>"
                  "<div><button type=button id=otains class=lt2 hidden>Installer n&aring;</button></div></div></fieldset>");
    p += snprintf(p, end - p,
                  "<fieldset><legend>Skjermbilder</legend>"
                  "<div class=chk><label><input type=checkbox name=scrctl value=1%s>"
                  "Tillat &aring; velge skjerm over nettet</label></div>"
                  "<small>For &aring; ta skjermbilde av en bestemt skjerm: <code>/screen</code> viser "
                  "skjermene, <code>/screen?vis=3</code> eller <code>/screen?sted=2&amp;type=tidevann</code> "
                  "bytter til en, og <code>/screen.png</code> er skjermbildet. Et bytte teller som et trykk "
                  "p&aring; skjermen.</small></fieldset>",
                  cfg->screen_ctl ? " checked" : "");
    p += snprintf(p, end - p, "%s%s", PAGE_MAINT, PAGE_SCRIPTS);
    if (p >= end - 1) {
        ESP_LOGE(TAG, "Setup page cut short (%u bytes)", (unsigned)cap);
    }
    return buf;
}

/* --------------------------------------------------------------------------
 * HTTP handlers
 * ------------------------------------------------------------------------ */

/* Equal strings, compared in a time that doesn't depend on where they
 * differ. */
static bool secret_equal(const char *a, const char *b)
{
    const size_t n = strlen(b);
    if (strlen(a) != n) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

/* Every page and request checks this first: HTTP Basic authentication with
 * the setup password and any user name, unless none is set. When refused,
 * the 401 that makes the browser ask has been sent. */
static bool authorized(httpd_req_t *req)
{
    if (s_web_pass[0] == '\0' || s_auth_bypass) {
        return true;
    }
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        unsigned char dec[144];
        size_t n = 0;
        if (strncasecmp(hdr, "Basic ", 6) == 0 &&
            mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = '\0';
            const char *colon = strchr((const char *)dec, ':');
            if (colon != NULL && secret_equal(colon + 1, s_web_pass)) {
                return true;
            }
        }
        ESP_LOGW(TAG, "Wrong setup page password");
        vTaskDelay(pdMS_TO_TICKS(1000)); /* slows down guessing */
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"MultiDisplay\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<meta charset=utf-8><p>Denne oppsettsiden har passord. Glemt det? Hold BOOT "
                            "inne mens skjermen sl&aring;s p&aring;, s&aring; &aring;pner oppsettnettet uten passord.");
    return false;
}

/* A POST must come from this page, not from another web site open in a
 * browser on the home network (which could otherwise change the WiFi or
 * start an update without a setup password): a browser names the page a
 * request comes from in Origin (or at least Referer), and its host must be
 * the one the request went to. Requests without either - curl - are let
 * through. When refused, the 403 has been sent. */
static bool same_origin(httpd_req_t *req)
{
    char host[64], from[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", from, sizeof(from)) != ESP_OK &&
        httpd_req_get_hdr_value_str(req, "Referer", from, sizeof(from)) != ESP_OK) {
        return true;
    }
    const char *h = strstr(from, "://");
    h = h ? h + 3 : from;
    const size_t len = strcspn(h, "/");
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && strlen(host) == len &&
        strncasecmp(h, host, len) == 0) {
        return true;
    }
    ESP_LOGW(TAG, "Refused a %s from another site (%s)", req->uri, from);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "Foresp\xC3\xB8rselen kom fra en annen side.");
    return false;
}

static esp_err_t h_root(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    app_config_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return httpd_resp_send_500(req);
    }
    app_config_load(cfg);
    char *page = build_page(cfg);
    free(cfg);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_sendstr(req, page);
    free(page);
    return err;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    httpd_resp_set_type(req, "application/json");

    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = PORTAL_MAX_SCAN;
    wifi_ap_record_t recs[PORTAL_MAX_SCAN];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return httpd_resp_sendstr(req, "[]");
    }

    char out[1024];
    char *p = out;
    char *e = out + sizeof(out);
    p += snprintf(p, e - p, "[");
    int emitted = 0;
    for (int i = 0; i < n && p < e - 64; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        p += snprintf(p, e - p, "%s{\"s\":\"", emitted ? "," : "");
        p = html_escape_append(p, e, (char *)recs[i].ssid); /* also fine for JSON quoting of "&<> */
        p += snprintf(p, e - p, "\",\"r\":%d}", recs[i].rssi);
        emitted++;
    }
    snprintf(p, e - p, "]");
    return httpd_resp_sendstr(req, out);
}

/* It may mark the running firmware as good first, which writes flash and
 * logs: 2 KB overflowed doing that (seen as a crash dump). Pinned to core 0,
 * where esp_restart() doesn't hang (see restart.c). */
#define REBOOT_TASK_STACK 4096

static void reboot_task(void *arg)
{
    (void)arg;
    /* A restart asked for on this page isn't a failing firmware: it booted,
     * connected and served the page. Keep it, or a save soon after an update
     * - before main.c's keep_firmware() - would go back to the old one. */
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Restart from the setup page - keeping this firmware");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

#define SAVE_BODY_MAX 12288

static esp_err_t save_form(httpd_req_t *req, const char *body);

static esp_err_t h_save(httpd_req_t *req)
{
    if (!authorized(req) || !same_origin(req)) {
        return ESP_OK;
    }
    char *body = malloc(SAVE_BODY_MAX);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }
    int total = 0;
    while (total < req->content_len && total < SAVE_BODY_MAX - 1) {
        int r = httpd_req_recv(req, body + total, SAVE_BODY_MAX - 1 - total);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        total += r;
    }
    body[total] = '\0';
    esp_err_t err = save_form(req, body);
    free(body);
    return err;
}

/* POST /ota: a firmware image (build/multi_display.bin) as the raw request
 * body - from the setup page, or e.g.
 *     curl --data-binary @build/multi_display.bin http://<ip>/ota
 * It goes into the app slot not running and is booted into; the bootloader
 * returns to the running firmware if the new one doesn't mark itself valid
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, see main.c). ota_writer checks it
 * is this project's firmware, signed with our key. Any version is accepted,
 * older ones too: this is how a bad release is undone by hand. */
#define OTA_CHUNK 4096

static esp_err_t ota_fail(httpd_req_t *req, const char *http_status, const char *msg)
{
    ESP_LOGE(TAG, "Firmware upload: %s", msg);
    status("Programvareoppdatering feilet");
    httpd_resp_set_status(req, http_status);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!authorized(req) || !same_origin(req)) {
        return ESP_OK;
    }
    uint8_t *buf = malloc(OTA_CHUNK);
    ota_writer_t *w = malloc(sizeof(*w));
    if (buf == NULL || w == NULL) {
        free(buf);
        free(w);
        return ota_fail(req, "500 Internal Server Error", "Ikke nok minne");
    }
    const char *err = ota_writer_start(w, req->content_len);
    if (err == NULL) {
        ESP_LOGI(TAG, "Firmware upload: %u bytes", (unsigned)req->content_len);
        status("Oppdaterer programvare...");
    }
    size_t done = 0;
    int timeouts = 0;
    while (err == NULL && done < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf, OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (r <= 0) {
            err = "Opplastingen ble avbrutt";
            ota_writer_abort(w);
            break;
        }
        err = ota_writer_write(w, buf, (size_t)r);
        done += (size_t)r;
    }
    if (err == NULL) {
        err = ota_writer_finish(w, NULL);
    }
    free(buf);
    free(w);
    if (err != NULL) {
        return ota_fail(req, "400 Bad Request", err);
    }
    ESP_LOGI(TAG, "Firmware update written - restarting");
    status("Programvare oppdatert.\nStarter p\xC3\xA5 nytt...");
    httpd_resp_sendstr(req, "Oppdatert - starter p\xC3\xA5 nytt\n");
    xTaskCreatePinnedToCore(reboot_task, "reboot", REBOOT_TASK_STACK, NULL, 5, NULL, 0);
    return ESP_OK;
}

/* "HH:MM" (as an <input type=time> sends it) to minutes after midnight, or
 * -1 if it isn't a valid time. */
/* "22:30", "22.30", "22,30", "2230" or "930" (phones' number pads often
 * have no ':') as minutes after midnight; -1 if it isn't a time. */
static int parse_hhmm(const char *s)
{
    int d[4], nd = 0, sep_at = -1;
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9' && nd < 4) {
            d[nd++] = *s - '0';
        } else if ((*s == ':' || *s == '.' || *s == ',') && sep_at < 0 && nd >= 1 && nd <= 2) {
            sep_at = nd;
        } else {
            return -1;
        }
    }
    const int hd = (sep_at >= 0) ? sep_at : nd - 2; /* hour digits; the minutes are the last two */
    if (nd - hd != 2 || hd < 1 || hd > 2) {
        return -1;
    }
    const int h = (hd == 2) ? d[0] * 10 + d[1] : d[0];
    const int m = d[hd] * 10 + d[hd + 1];
    return (h > 23 || m > 59) ? -1 : h * 60 + m;
}

static esp_err_t save_form_into(httpd_req_t *req, const char *body, app_config_t *cfg)
{
    app_config_load(cfg); /* keep the WiFi fields at their current value if omitted */
    /* A blank password keeps the saved one - the page never shows it - unless
     * the network changed: then blank means an open network. */
    char old_ssid[sizeof(cfg->wifi_ssid)];
    snprintf(old_ssid, sizeof(old_ssid), "%s", cfg->wifi_ssid);
    form_field(body, "ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    char pass[sizeof(cfg->wifi_pass)];
    if (form_field(body, "pass", pass, sizeof(pass)) &&
        (pass[0] != '\0' || strcmp(old_ssid, cfg->wifi_ssid) != 0)) {
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", pass);
    }
    /* The setup page's own password: blank keeps it, the checkbox removes it. */
    char val[4];
    if (form_field(body, "webpassoff", val, sizeof(val))) {
        cfg->web_pass[0] = '\0';
    } else if (form_field(body, "webpass", pass, sizeof(pass)) && pass[0] != '\0') {
        snprintf(cfg->web_pass, sizeof(cfg->web_pass), "%s", pass);
    }
    char theme[4];
    if (form_field(body, "theme", theme, sizeof(theme))) {
        cfg->theme = (strcmp(theme, "1") == 0) ? APP_THEME_DARK : APP_THEME_LIGHT;
    }
    /* The checkbox is only sent when ticked, so go by the time fields, which
     * the page always sends. */
    char hhmm[8];
    if (form_field(body, "dimstart", hhmm, sizeof(hhmm))) {
        char val[4];
        int m;
        cfg->dim_enabled = form_field(body, "dimon", val, sizeof(val)) ? 1 : 0;
        if (form_field(body, "nightoff", val, sizeof(val))) {
            cfg->night_off = (strcmp(val, "1") == 0) ? 1 : 0;
        }
        if ((m = parse_hhmm(hhmm)) >= 0) {
            cfg->dim_start = (uint16_t)m;
        }
        if (form_field(body, "dimend", hhmm, sizeof(hhmm)) && (m = parse_hhmm(hhmm)) >= 0) {
            cfg->dim_end = (uint16_t)m;
        }
    }
    /* Fonts: again the checkboxes are only sent when ticked, so go by the
     * size fields; an out-of-range size keeps the current one. */
    char px[8];
    if (form_field(body, "titlepx", px, sizeof(px))) {
        char val[4];
        long v = strtol(px, NULL, 10);
        if (v >= APP_CONFIG_TITLE_PX_MIN && v <= APP_CONFIG_TITLE_PX_MAX) {
            cfg->title_px = (uint8_t)v;
        }
        if (form_field(body, "textpx", px, sizeof(px)) &&
            (v = strtol(px, NULL, 10)) >= APP_CONFIG_TEXT_PX_MIN && v <= APP_CONFIG_TEXT_PX_MAX) {
            cfg->text_px = (uint8_t)v;
        }
        cfg->title_bold = form_field(body, "titlebold", val, sizeof(val)) ? 1 : 0;
        cfg->text_bold = form_field(body, "textbold", val, sizeof(val)) ? 1 : 0;
    }
    /* Rotation: again go by the number fields, which are always sent. */
    char num[8];
    if (form_field(body, "autoidle", num, sizeof(num))) {
        char val[4];
        long v = strtol(num, NULL, 10);
        cfg->auto_idle_min = (v > 0 && v <= APP_CONFIG_AUTO_IDLE_MIN_MAX) ? (uint16_t)v : 0;
        if (form_field(body, "autodwell", num, sizeof(num)) &&
            (v = strtol(num, NULL, 10)) >= APP_CONFIG_AUTO_DWELL_S_MIN && v <= APP_CONFIG_AUTO_DWELL_S_MAX) {
            cfg->auto_dwell_s = (uint16_t)v;
        }
        cfg->auto_overview = form_field(body, "autoov", val, sizeof(val)) ? 1 : 0;
        cfg->ov_show = form_field(body, "ovshow", val, sizeof(val)) ? 1 : 0;
        cfg->auto_night_pause = form_field(body, "autonight", val, sizeof(val)) ? 1 : 0;
    }
    /* The display's name: made into a host name; blank keeps the old one. */
    char devname[APP_CONFIG_DEVNAME_MAX * 2];
    if (form_field(body, "devname", devname, sizeof(devname))) {
        char host[APP_CONFIG_DEVNAME_MAX];
        app_config_hostname(devname, host, sizeof(host));
        if (host[0]) {
            snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", host);
        }
    }
    /* Firmware updates: the address is always sent, the checkbox only when
     * ticked. Blank turns checking off; anything but http(s) is refused. */
    char url[APP_CONFIG_OTA_URL_MAX];
    if (form_field(body, "otaurl", url, sizeof(url))) {
        char val[4];
        cfg->ota_auto = form_field(body, "otaauto", val, sizeof(val)) ? 1 : 0;
        cfg->screen_ctl = form_field(body, "scrctl", val, sizeof(val)) ? 1 : 0; /* in the same section */
        char hhmm[8];
        int m;
        if (form_field(body, "otaat", hhmm, sizeof(hhmm)) && (m = parse_hhmm(hhmm)) >= 0) {
            cfg->ota_at = (uint16_t)m;
        }
        if (form_field(body, "otaevery", val, sizeof(val))) {
            long v = strtol(val, NULL, 10);
            if (v >= APP_CONFIG_OTA_EVERY_H_MIN && v <= APP_CONFIG_OTA_EVERY_H_MAX) {
                cfg->ota_every_h = (uint8_t)v;
            }
        }
        if (url[0] == '\0' || strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
            snprintf(cfg->ota_url, sizeof(cfg->ota_url), "%s", url);
        }
    }
    /* Likewise a blank secret keeps the saved one, unless the ID is gone. */
    form_field(body, "aisid", cfg->ais_client_id, sizeof(cfg->ais_client_id));
    char secret[sizeof(cfg->ais_client_secret)];
    if (form_field(body, "aissec", secret, sizeof(secret)) && secret[0] != '\0') {
        snprintf(cfg->ais_client_secret, sizeof(cfg->ais_client_secret), "%s", secret);
    } else if (cfg->ais_client_id[0] == '\0') {
        cfg->ais_client_secret[0] = '\0';
    }
    if (form_field(body, "yremail", cfg->yr_email, sizeof(cfg->yr_email)) &&
        !app_config_email_valid(cfg->yr_email)) {
        cfg->yr_email[0] = '\0'; /* blank or malformed: fall back to the default */
    }
    /* The calendar: its address fields are always sent, the checkboxes
     * only when ticked. A blank address keeps the saved one. */
    char *cal = malloc(APP_CONFIG_CAL_URL_MAX);
    if (cal == NULL) {
        return httpd_resp_send_500(req);
    }
    if (form_field(body, "calurl0", cal, APP_CONFIG_CAL_URL_MAX)) {
        char val[4];
        cfg->cal_show = form_field(body, "calshow", val, sizeof(val)) ? 1 : 0;
        cfg->cal_rotate = form_field(body, "calrot", val, sizeof(val)) ? 1 : 0;
        for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
            char key[] = "calurl0", off[] = "caloff0";
            key[6] = off[6] = (char)('0' + i);
            if (form_field(body, off, val, sizeof(val))) {
                cfg->cal_url[i][0] = '\0';
            } else if (form_field(body, key, cal, APP_CONFIG_CAL_URL_MAX) && cal[0] != '\0') {
                if (!app_config_cal_url_valid(cal)) {
                    free(cal);
                    httpd_resp_set_type(req, "text/html");
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, "<meta charset=utf-8><p>Ugyldig kalenderadresse: den m&aring; "
                                                   "begynne med https://, http:// eller webcal://."
                                                   "<p><a href=/>Tilbake</a>");
                }
                snprintf(cfg->cal_url[i], sizeof(cfg->cal_url[i]), "%s", cal);
            }
        }
    }
    free(cal);

    httpd_resp_set_type(req, "text/html");
    if (cfg->wifi_ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "<meta charset=utf-8><p>Ugyldig: et WiFi-nett m&aring; oppgis."
            "<p><a href=/>Tilbake</a>");
    }

    /* Collect the numbered location blocks (name0/lat0/lon0, ...). A block
     * with all three fields empty is skipped; the rest are compacted so the
     * stored list has no gaps. */
    app_location_t locs[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint8_t show[APP_CONFIG_MAX_LOCATIONS] = { 0 }; /* compacted like the locations */
    uint8_t auto_show[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t radar_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t ship_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t ship_min_len[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t ship_near_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t ship_near_len[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t rain_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    char (*deps)[APP_CONFIG_DEPARTURES_MAX] = calloc(APP_CONFIG_MAX_LOCATIONS, APP_CONFIG_DEPARTURES_MAX);
    if (deps == NULL) {
        return httpd_resp_send_500(req);
    }
    int n = 0;
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        char key[16];
        char name[APP_CONFIG_NAME_MAX];
        char lat[APP_CONFIG_COORD_MAX];
        char lon[APP_CONFIG_COORD_MAX];

        snprintf(key, sizeof(key), "name%d", i);
        form_field(body, key, name, sizeof(name));
        snprintf(key, sizeof(key), "lat%d", i);
        form_field(body, key, lat, sizeof(lat));
        snprintf(key, sizeof(key), "lon%d", i);
        form_field(body, key, lon, sizeof(lon));

        if (name[0] == '\0' && lat[0] == '\0' && lon[0] == '\0') {
            continue;
        }
        if (!app_config_coord_valid(lat, true) || !app_config_coord_valid(lon, false)) {
            free(deps);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req,
                "<meta charset=utf-8><p>Ugyldig: hvert sted m&aring; ha en "
                "breddegrad innenfor &plusmn;90 og en lengdegrad innenfor &plusmn;180."
                "<p><a href=/>Tilbake</a>");
        }
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "Sted %d", n + 1);
        }
        snprintf(locs[n].name, sizeof(locs[n].name), "%s", name);
        snprintf(locs[n].lat, sizeof(locs[n].lat), "%s", lat);
        snprintf(locs[n].lon, sizeof(locs[n].lon), "%s", lon);

        /* Which screens (checkboxes: only ticked ones are sent) and the
         * ranges; nothing ticked or out of range falls back to weather / the
         * default range. */
        char val[8];
        uint8_t sh = 0;
        snprintf(key, sizeof(key), "wx%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_WEATHER : 0;
        snprintf(key, sizeof(key), "ac%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_RADAR : 0;
        snprintf(key, sizeof(key), "sh%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_SHIPS : 0;
        snprintf(key, sizeof(key), "rn%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_RAIN : 0;
        snprintf(key, sizeof(key), "dp%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_DEPARTURES : 0;
        snprintf(key, sizeof(key), "uk%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_WEEK : 0;
        snprintf(key, sizeof(key), "lq%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_AIR : 0;
        snprintf(key, sizeof(key), "td%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_TIDE : 0;
        snprintf(key, sizeof(key), "dep%d", i);
        form_field(body, key, deps[n], APP_CONFIG_DEPARTURES_MAX);
        show[n] = sh ? sh : APP_SHOW_WEATHER;
        static const struct { const char *key; uint8_t bit; } rot[] = {
            { "aw%d", APP_SHOW_WEATHER }, { "aa%d", APP_SHOW_RADAR }, { "as%d", APP_SHOW_SHIPS },
            { "ar%d", APP_SHOW_RAIN }, { "ad%d", APP_SHOW_DEPARTURES }, { "al%d", APP_SHOW_AIR }, { "au%d", APP_SHOW_WEEK },
            { "at%d", APP_SHOW_TIDE },
        };
        for (int r = 0; r < (int)(sizeof(rot) / sizeof(rot[0])); r++) {
            snprintf(key, sizeof(key), rot[r].key, i);
            auto_show[n] |= form_field(body, key, val, sizeof(val)) ? rot[r].bit : 0;
        }
        snprintf(key, sizeof(key), "radarkm%d", i);
        long km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        radar_km[n] = (km >= APP_CONFIG_RADAR_KM_MIN && km <= APP_CONFIG_RADAR_KM_MAX)
                          ? (uint16_t)km : APP_CONFIG_RADAR_KM_DEFAULT;
        snprintf(key, sizeof(key), "shipkm%d", i);
        km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        ship_km[n] = (km >= APP_CONFIG_SHIP_KM_MIN && km <= APP_CONFIG_SHIP_KM_MAX)
                         ? (uint16_t)km : APP_CONFIG_SHIP_KM_DEFAULT;
        snprintf(key, sizeof(key), "shipminlen%d", i);
        long len = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        ship_min_len[n] = (len > 0 && len <= APP_CONFIG_SHIP_MIN_LEN_MAX) ? (uint16_t)len : 0;
        snprintf(key, sizeof(key), "shipnearkm%d", i);
        km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        ship_near_km[n] = (km > 0 && km <= APP_CONFIG_SHIP_NEAR_KM_MAX) ? (uint16_t)km : 0;
        snprintf(key, sizeof(key), "shipnearlen%d", i);
        len = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        ship_near_len[n] = (len > 0 && len <= APP_CONFIG_SHIP_MIN_LEN_MAX) ? (uint16_t)len : 0;
        snprintf(key, sizeof(key), "rainkm%d", i);
        km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        rain_km[n] = (km >= APP_CONFIG_RAIN_KM_MIN && km <= APP_CONFIG_RAIN_KM_MAX)
                         ? (uint16_t)km : APP_CONFIG_RAIN_KM_DEFAULT;
        n++;
    }

    if (n == 0) {
        /* Nothing entered - fall back to the compiled-in default location. */
        snprintf(locs[0].name, sizeof(locs[0].name), "%s", CONFIG_EXAMPLE_YR_LOCATION_NAME);
        snprintf(locs[0].lat, sizeof(locs[0].lat), "%s", CONFIG_EXAMPLE_YR_LATITUDE);
        snprintf(locs[0].lon, sizeof(locs[0].lon), "%s", CONFIG_EXAMPLE_YR_LONGITUDE);
        show[0] = APP_SHOW_WEATHER;
        radar_km[0] = APP_CONFIG_RADAR_KM_DEFAULT;
        ship_km[0] = APP_CONFIG_SHIP_KM_DEFAULT;
        rain_km[0] = APP_CONFIG_RAIN_KM_DEFAULT;
        n = 1;
    }

    memcpy(cfg->locations, locs, sizeof(cfg->locations));
    cfg->location_count = (uint8_t)n;
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        cfg->show[i] = show[i] ? show[i] : APP_SHOW_WEATHER;
        cfg->auto_show[i] = auto_show[i];
        cfg->radar_km[i] = radar_km[i] ? radar_km[i] : APP_CONFIG_RADAR_KM_DEFAULT;
        cfg->ship_km[i] = ship_km[i] ? ship_km[i] : APP_CONFIG_SHIP_KM_DEFAULT;
        cfg->ship_min_len_m[i] = ship_min_len[i];
        cfg->ship_near_km[i] = ship_near_km[i];
        cfg->ship_near_min_len_m[i] = ship_near_len[i];
        cfg->rain_km[i] = rain_km[i] ? rain_km[i] : APP_CONFIG_RAIN_KM_DEFAULT;
    }
    memcpy(cfg->departures, deps, sizeof(cfg->departures));
    free(deps);

    if (app_config_save(cfg) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<p style='font-family:system-ui;max-width:24rem;margin:3rem auto;text-align:center'>"
        "Lagret. Starter p&aring; nytt&hellip;"
        "<p style='font-family:system-ui;text-align:center'>"
        "<a href=/>Tilbake til oppsettsiden</a></p>");
    xTaskCreatePinnedToCore(reboot_task, "reboot", REBOOT_TASK_STACK, NULL, 5, NULL, 0);
    return ESP_OK;
}

/* The app_config_t goes on the heap: it's a couple of KB with the departure
 * selections, too much for the httpd task's stack next to everything else. */
static esp_err_t save_form(httpd_req_t *req, const char *body)
{
    app_config_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = save_form_into(req, body, cfg);
    free(cfg);
    return err;
}

/* GET /config.json: the settings as a backup file, without the WiFi network
 * and the secrets (app_config_json.c). */
static esp_err_t h_config_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    app_config_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return httpd_resp_send_500(req);
    }
    app_config_load(cfg);
    char *json = app_config_to_json(cfg);
    free(cfg);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"multidisplay-innstillinger.json\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* POST /config.json: restore such a backup over the current settings (the
 * WiFi and secrets stay), save and restart. */
static esp_err_t h_config_put(httpd_req_t *req)
{
    if (!authorized(req) || !same_origin(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len >= SAVE_BODY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Filen er tom eller for stor.");
    }
    char *body = malloc(SAVE_BODY_MAX);
    app_config_t *cfg = malloc(sizeof(*cfg));
    if (body == NULL || cfg == NULL) {
        free(body);
        free(cfg);
        return httpd_resp_send_500(req);
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, req->content_len - total);
        if (r <= 0) {
            free(body);
            free(cfg);
            return httpd_resp_send_500(req);
        }
        total += r;
    }
    body[total] = '\0';
    app_config_load(cfg);
    char why[96];
    const bool ok = app_config_from_json(body, cfg, why, sizeof(why)) && app_config_save(cfg) == ESP_OK;
    free(body);
    free(cfg);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, why);
    }
    ESP_LOGI(TAG, "Settings restored from a backup - restarting");
    httpd_resp_sendstr(req, "Innstillingene er gjenopprettet. Starter p\xC3\xA5 nytt...");
    xTaskCreatePinnedToCore(reboot_task, "reboot", REBOOT_TASK_STACK, NULL, 5, NULL, 0);
    return ESP_OK;
}

/* An embedded text file (EMBED_TXTFILES in main/CMakeLists.txt). */
static esp_err_t send_asset(httpd_req_t *req, const char *type, const char *text)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, text);
}

/* The departure picker (components/setup_departures.js). */
static esp_err_t h_dep_js(httpd_req_t *req)
{
    extern const char dep_js_start[] asm("_binary_setup_departures_js_start");
    return send_asset(req, "text/javascript", dep_js_start);
}

/* The setup page's look and behaviour (components/setup_page.css/.js). */
static esp_err_t h_setup_css(httpd_req_t *req)
{
    extern const char css_start[] asm("_binary_setup_page_css_start");
    return send_asset(req, "text/css", css_start);
}

static esp_err_t h_setup_js(httpd_req_t *req)
{
    extern const char js_start[] asm("_binary_setup_page_js_start");
    return send_asset(req, "text/javascript", js_start);
}

/* Any other GET (captive-portal probes: /generate_204, /hotspot-detect.html,
 * ...) just gets the setup page. */
static esp_err_t h_catchall(httpd_req_t *req)
{
    return h_root(req);
}

static void start_web_server(void)
{
    if (s_httpd) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* The POST body and the app_config_t are on the heap (see h_save). */
    config.stack_size = 6144;
    /* The stack stays in internal RAM (the default task_caps): the update
     * and restore handlers call the OTA functions, which memory-map flash
     * and assert on a PSRAM stack (see main.c, yr_weather). */
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return;
    }
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/save", .method = HTTP_POST, .handler = h_save });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/ota", .method = HTTP_POST, .handler = h_ota });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/scan", .method = HTTP_GET, .handler = h_scan });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/dep.js", .method = HTTP_GET, .handler = h_dep_js });
    httpd_register_uri_handler(s_httpd,
                               &(httpd_uri_t){ .uri = "/setup.css", .method = HTTP_GET, .handler = h_setup_css });
    httpd_register_uri_handler(s_httpd,
                               &(httpd_uri_t){ .uri = "/setup.js", .method = HTTP_GET, .handler = h_setup_js });
    httpd_register_uri_handler(s_httpd,
                               &(httpd_uri_t){ .uri = "/config.json", .method = HTTP_GET, .handler = h_config_get });
    httpd_register_uri_handler(s_httpd,
                               &(httpd_uri_t){ .uri = "/config.json", .method = HTTP_POST, .handler = h_config_put });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = h_root });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = h_catchall });
}

/* An added handler, behind the same password as the setup page. */
static esp_err_t h_added(httpd_req_t *req)
{
    if (!authorized(req) || (req->method == HTTP_POST && !same_origin(req))) {
        return ESP_OK;
    }
    esp_err_t (*handler)(httpd_req_t *) = req->user_ctx;
    return handler(req);
}

esp_err_t wifi_provision_add_get_handler(const char *uri, esp_err_t (*handler)(httpd_req_t *req))
{
    if (s_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Wildcard URIs match in registration order: move the catch-all last. */
    httpd_unregister_uri_handler(s_httpd, "/*", HTTP_GET);
    esp_err_t err = httpd_register_uri_handler(
        s_httpd, &(httpd_uri_t){ .uri = uri, .method = HTTP_GET, .handler = h_added, .user_ctx = handler });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = h_catchall });
    return err;
}

esp_err_t wifi_provision_add_post_handler(const char *uri, esp_err_t (*handler)(httpd_req_t *req))
{
    if (s_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return httpd_register_uri_handler(
        s_httpd, &(httpd_uri_t){ .uri = uri, .method = HTTP_POST, .handler = h_added, .user_ctx = handler });
}

/* --------------------------------------------------------------------------
 * Captive-portal DNS: answer every A query with the portal IP
 * ------------------------------------------------------------------------ */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ip4_addr_t portal_ip;
    ip4addr_aton(PORTAL_AP_IP, &portal_ip);

    uint8_t pkt[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (len < 12) {
            continue;
        }
        /* Turn the query into a response: QR=1, RA=1, one answer. */
        pkt[2] |= 0x80;
        pkt[3] = 0x80;
        pkt[6] = 0; pkt[7] = 1;   /* ANCOUNT = 1 */
        pkt[8] = 0; pkt[9] = 0;   /* NSCOUNT */
        pkt[10] = 0; pkt[11] = 0; /* ARCOUNT */
        if (len + 16 > (int)sizeof(pkt)) {
            continue;
        }
        uint8_t *a = pkt + len;
        *a++ = 0xC0; *a++ = 0x0C;              /* name -> pointer to the question */
        *a++ = 0x00; *a++ = 0x01;              /* type A */
        *a++ = 0x00; *a++ = 0x01;              /* class IN */
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C; /* TTL 60 */
        *a++ = 0x00; *a++ = 0x04;              /* RDLENGTH */
        memcpy(a, &portal_ip.addr, 4);
        a += 4;
        sendto(sock, pkt, a - pkt, 0, (struct sockaddr *)&from, flen);
    }
}

/* --------------------------------------------------------------------------
 * WiFi
 * ------------------------------------------------------------------------ */

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_stop_reconnect) {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        if (!s_stop_reconnect) {
            wifi_event_sta_disconnected_t *e = data;
            s_retries++;
            ESP_LOGW(TAG, "WiFi disconnected (reason %d), retrying (attempt %d)...",
                     e ? e->reason : -1, s_retries);
            /* Back off before trying again - most useful right after
             * power-on, when the AP itself may still be booting (e.g. after a
             * power outage) and every immediate retry fails the same way for
             * seconds at a time; also keeps a fast retry storm from adding to
             * the DRAM pressure already tight at boot. From a timer, not a
             * sleep here, which would hold up the shared event loop. */
            int shift = s_retries < 6 ? s_retries - 1 : 5;
            uint64_t delay_ms = (uint64_t)RECONNECT_FIRST_MS << shift;
            if (delay_ms > RECONNECT_MAX_MS) {
                delay_ms = RECONNECT_MAX_MS;
            }
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, delay_ms * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_sta_up = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static bool boot_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    /* debounce: require it low for ~150 ms */
    for (int i = 0; i < 15; i++) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void (*s_before_connect)(void);

void wifi_provision_before_connect(void (*fn)(void))
{
    s_before_connect = fn;
}

static void net_common_init(void)
{
    s_events = xEventGroupCreate();
    const esp_timer_create_args_t rt = { .callback = reconnect_timer_cb, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MultiDisplay-%02X%02X", mac[4], mac[5]);
    if (s_before_connect != NULL) {
        s_before_connect();
    }
}

static bool sta_try_connect(const app_config_t *cfg)
{
    s_stop_reconnect = false;
    s_retries = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED);

    wifi_config_t wc = { 0 }; /* zeroed, so a short copy is NUL-padded */
    memcpy(wc.sta.ssid, cfg->wifi_ssid,
           strnlen(cfg->wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cfg->wifi_pass,
           strnlen(cfg->wifi_pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* The name the router lists it under (DHCP), like its .local name. */
    esp_netif_set_hostname(s_sta_netif, s_hostname);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID '%s'...", cfg->wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & BIT_CONNECTED) {
        return true;
    }
    s_stop_reconnect = true;
    esp_wifi_disconnect();
    return false;
}

/* How often the portal tries the saved network again (see portal_run). */
#define PORTAL_STA_RETRY_MS     60000

/* Run the setup portal until the form is saved (h_save reboots). With
 * `retry_sta` - a provisioned device whose network didn't answer, typically
 * because the router is still booting after a power cut - the saved network
 * is also tried again every PORTAL_STA_RETRY_MS, and the device restarts
 * into normal operation once it connects. Not while someone is on the
 * portal's own network: a connect attempt hops channels and would drop them. */
static void portal_run(bool retry_sta)
{
    s_stop_reconnect = true;
    xEventGroupClearBits(s_events, BIT_CONNECTED);

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* STA up too, so /scan works */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup portal: SSID '%s' -> http://%s/", s_ap_ssid, PORTAL_AP_IP);

    char msg[160];
    snprintf(msg, sizeof(msg), "Oppsett:\nKoble til WiFi \"%s\"\nog \xC3\xA5pne  http://%s%s", s_ap_ssid, PORTAL_AP_IP,
             retry_sta ? "\n\nPr\xC3\xB8ver lagret WiFi igjen hvert minutt" : "");
    status(msg);

    xTaskCreateWithCaps(dns_task, "captdns", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    start_web_server();

    /* Otherwise nothing more to do here - h_save reboots the device once the
     * form is submitted. */
    TickType_t last_try = xTaskGetTickCount();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!retry_sta) {
            continue;
        }
        if (xEventGroupGetBits(s_events) & BIT_CONNECTED) {
            ESP_LOGI(TAG, "Saved network is back - restarting");
            status("WiFi tilkoblet \xE2\x80\x93 starter p\xC3\xA5 nytt...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            restart_device();
        }
        wifi_sta_list_t clients;
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
            last_try = xTaskGetTickCount(); /* someone is setting it up: leave them be */
            continue;
        }
        if (xTaskGetTickCount() - last_try >= pdMS_TO_TICKS(PORTAL_STA_RETRY_MS)) {
            last_try = xTaskGetTickCount();
            ESP_LOGI(TAG, "Trying the saved network again...");
            esp_wifi_connect();
        }
    }
}

/* Announce the setup page as http://<name>.local/ on the home network, the
 * name set on the setup page (so several displays can share a network). */
static void mdns_announce(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS didn't start");
        return;
    }
    char instance[48];
    snprintf(instance, sizeof(instance), "MultiDisplay %s", s_hostname);
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(instance);
    mdns_service_add(instance, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "Announced as %s.local", s_hostname);
}

esp_err_t wifi_provision_connect(const app_config_t *cfg, wifi_provision_status_fn status_fn)
{
    s_status = status_fn;

    net_common_init();

    bool force_portal = boot_button_held();
    snprintf(s_web_pass, sizeof(s_web_pass), "%s", cfg->web_pass);
    snprintf(s_hostname, sizeof(s_hostname), "%s", cfg->device_name);
    s_auth_bypass = force_portal;
    if (force_portal) {
        ESP_LOGW(TAG, "BOOT held - forcing setup portal");
    }

    if (!force_portal && app_config_is_provisioned()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Kobler til %s...", cfg->wifi_ssid);
        status(msg);
        if (sta_try_connect(cfg)) {
            status("");
            start_web_server(); /* reachable on the station IP for later edits */
            mdns_announce();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi connect failed - opening setup portal");
        status("WiFi feilet \xE2\x80\x93 starter oppsett");
        esp_wifi_stop();
        portal_run(true); /* never returns */
    }

    portal_run(false); /* never returns */
    return ESP_OK;
}

bool wifi_provision_is_up(void)
{
    return s_sta_up;
}

const char *wifi_provision_get_ip(void)
{
    return s_sta_ip; /* "" until the station has an IP */
}
