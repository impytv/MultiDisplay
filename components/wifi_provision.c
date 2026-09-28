#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "wifi_provision.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
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

static const char PAGE_HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<html lang=no><title>MultiDisplay oppsett</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:26rem;margin:2rem auto;padding:0 1rem;background:#f6f6f4;color:#222}"
    "h1{font-size:1.3rem}label{display:block;margin:.8rem 0 .2rem;font-weight:600}"
    "input{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem;border:1px solid #bbb;border-radius:.4rem}"
    "button{margin-top:1.3rem;width:100%;padding:.7rem;font-size:1rem;border:0;border-radius:.4rem;background:#2d5a86;color:#fff}"
    ".row{display:flex;gap:.6rem}.row>div{flex:1}small{color:#666}"
    "fieldset{margin:.9rem 0;padding:.2rem .8rem .8rem;border:1px solid #ccc;border-radius:.5rem}"
    "legend{padding:0 .4rem;color:#555;font-weight:600}"
    ".mv{width:auto;margin:0 0 0 .4rem;padding:.05rem .5rem;font-size:.85rem;background:#e4e8ee;color:#2d5a86}"
    ".mv:disabled{opacity:.35}"
    "select{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem;border:1px solid #bbb;border-radius:.4rem;background:#fff}"
    ".chk{display:flex;gap:1.2rem;flex-wrap:wrap}.chk label{display:flex;align-items:center;gap:.35rem;margin:.2rem 0;font-weight:400}"
    ".chk input{width:auto;margin:0}"
    ".hits button{display:block;margin:.3rem 0 0;padding:.45rem .6rem;text-align:left;background:#e4e8ee;color:#222}"
    ".hits small{color:#555}"
    "</style><h1>MultiDisplay oppsett</h1><form method=post action=/save>";

static const char PAGE_TAIL[] =
    "<button type=submit>Lagre og start p&aring; nytt</button></form>"
    /* Firmware update: the .bin goes up as the raw body of POST /ota. */
    "<fieldset><legend>Last opp programvare</legend><small>build/multi_display.bin fra "
    "prosjektet, signert med prosjektets n&oslash;kkel. Skjermen starter p&aring; nytt med den nye "
    "programvaren, og g&aring;r tilbake til den forrige om den nye ikke starter som den skal. "
    "Eldre versjoner godtas ogs&aring;.</small>"
    "<input type=file id=fw accept=.bin style='margin-top:.6rem'>"
    "<button type=button id=fwb>Last opp og start p&aring; nytt</button><small id=fws></small></fieldset>"
    /* Settings backup: /config.json down, and back up to restore it. */
    "<fieldset><legend>Sikkerhetskopi</legend><small>Alle innstillingene unntatt WiFi og passord/"
    "hemmeligheter, som en fil. Gjenoppretting beholder skjermens WiFi og passord, og starter "
    "den p&aring; nytt.</small><p><a href=/config.json download>Last ned innstillingene</a></p>"
    "<input type=file id=cfgf accept=.json><button type=button id=cfgb>Gjenopprett</button>"
    "<small id=cfgs></small></fieldset>"
    /* Health (main/diag.c): /status, /log and /coredump. */
    "<fieldset><legend>Driftsstatus</legend><div id=diag><small>Henter...</small></div>"
    "<p><small><a href=/log target=_blank>Logg</a> &middot; <a href=/screen.png target=_blank>Skjermbilde</a>"
    "<span id=cdl hidden> &middot; <a href=/coredump>Krasjdump</a> &middot; "
    "<a href=# id=cde>slett den</a></span></small></p></fieldset>"
    "<script>function E(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML}"
    "fwb.onclick=()=>{let f=fw.files[0];if(!f)return;fwb.disabled=true;"
    "fws.textContent='Laster opp...';let x=new XMLHttpRequest();x.open('POST','/ota');"
    "x.upload.onprogress=e=>fws.textContent='Laster opp '+Math.round(100*e.loaded/e.total)+' %';"
    "x.onload=()=>{fws.textContent=x.responseText;fwb.disabled=x.status==200};"
    "x.onerror=()=>{fws.textContent='Opplastingen feilet';fwb.disabled=false};x.send(f)};"
    "function D(){fetch('/status').then(r=>r.json()).then(s=>{let u=s.oppetid_s,t=u>=86400?Math.floor(u/86400)+' d ':'';"
    "t+=Math.floor(u%86400/3600)+' t '+Math.floor(u%3600/60)+' min';"
    "let h='<small>Versjon '+E(s.versjon)+', oppe i '+t+'. Sist startet av: '+E(s.omstart)+'.';"
    "if(s.wifi_dbm!==undefined)h+=' WiFi '+s.wifi_dbm+' dBm.';h+=' Klokka: '+E(s.klokke)+'.';"
    "h+=' Minne: '+Math.round(s.minne.intern_ledig/1024)+' KB internt (lavest '+Math.round(s.minne.intern_lavest/1024)+"
    "'), '+Math.round(s.minne.psram_ledig/1024)+' KB PSRAM.';"
    "if(s.krasjdump)h+=' <b>Krasjdump lagret'+(s.krasjgrunn?': '+E(s.krasjgrunn):'')+'.</b>';h+='</small>';"
    "s.tjenester.forEach(v=>{h+='<br><small>'+(v.feiler?'&#9888; ':'&#10003; ')+E(v.navn)+(v.sist_ok=='aldri'?': aldri hentet':': ok '+E(v.sist_ok))+"
    "(v.feiler?', feil '+E(v.sist_feil)+' ('+E(v.feil)+')':'')+'</small>'});"
    "diag.innerHTML=h;cdl.hidden=!s.krasjdump}).catch(e=>{diag.innerHTML='<small>Ikke tilgjengelig i oppsettmodus.</small>'})}"
    "cde.onclick=e=>{e.preventDefault();fetch('/coredump/erase',{method:'POST'}).then(D)};D();"
    "cfgb.onclick=()=>{let f=cfgf.files[0];if(!f)return;cfgb.disabled=true;cfgs.textContent='Gjenoppretter...';"
    "fetch('/config.json',{method:'POST',body:f}).then(r=>r.text().then(t=>{cfgs.textContent=t;cfgb.disabled=r.ok}))"
    ".catch(e=>{cfgs.textContent='Feilet';cfgb.disabled=false})};"
    /* Firmware updates: the status from main/updater.c, polled while a
     * check or install runs. */
    "function U(){fetch('/ota/status').then(r=>r.json()).then(s=>{"
    "let t='Kjører '+s.running+'. ';"
    "if(s.busy)t+=s.progress>=0?'Installerer '+s.available.version+': '+s.progress+' %':'Sjekker...';"
    "else if(s.checked)t+='Sist sjekket '+s.checked+': '+s.result;else t+='Ikke sjekket ennå.';"
    "otast.innerHTML='<small>'+E(t)+'</small>';"
    "let a=s.available;otanew.innerHTML=a?'<p style=\\'margin:.4rem 0 0\\'><b>'+E(a.version)+'</b>'+"
    "(a.released?' ('+E(a.released)+')':'')+(a.notes?': '+E(a.notes):'')+'</p>':'';"
    "otains.hidden=!a||s.busy;otachk.disabled=s.busy;if(s.busy)setTimeout(U,1500)})"
    ".catch(e=>{otast.innerHTML='<small>Ikke tilgjengelig i oppsettmodus.</small>';otachk.disabled=true})}"
    "function P(u){otachk.disabled=true;otains.hidden=true;fetch(u,{method:'POST'}).then(()=>setTimeout(U,800))}"
    "otachk.onclick=()=>P('/ota/check');otains.onclick=()=>{if(confirm('Installere nå? Skjermen starter på nytt.'))"
    "P('/ota/install')};U()</script>"
    "<script>fetch('/scan').then(r=>r.json()).then(l=>{let d=document.getElementById('nets');"
    "l.forEach(n=>{let o=document.createElement('option');o.value=n.s;d.appendChild(o)})}).catch(e=>{});</script>"
    /* Place search (Kartverket's place names): fills in a location's name,
     * latitude and longitude. */
    "<script>document.querySelectorAll('fieldset[data-loc]').forEach(f=>{let q=f.querySelector('.plq'),"
    "h=f.querySelector('.hits'),t,c;q.oninput=()=>{clearTimeout(t);if(c)c.abort();let v=q.value.trim();"
    "if(v.length<2){h.innerHTML='';return}t=setTimeout(()=>{c=new AbortController();"
    "fetch('https://ws.geonorge.no/stedsnavn/v1/navn?fuzzy=true&utkoordsys=4258&treffPerSide=8&side=1&sok='+"
    "encodeURIComponent(v),{signal:c.signal}).then(r=>r.json()).then(d=>{h.innerHTML='';"
    "(d.navn||[]).forEach(n=>{let p=n.representasjonspunkt,b=document.createElement('button');b.type='button';"
    "b.innerHTML=E(n['skrivemåte'])+' <small>'+E(n.navneobjekttype)+', '+"
    "E((n.kommuner||[]).map(k=>k.kommunenavn).join(', '))+'</small>';"
    "b.onclick=()=>{let i=f.dataset.loc;f.querySelector('[name=name'+i+']').value=n['skrivemåte'];"
    "f.querySelector('[name=lat'+i+']').value=p.nord.toFixed(4);f.querySelector('[name=lon'+i+']').value=p['øst'].toFixed(4);"
    "h.innerHTML='';q.value=''};h.appendChild(b)});if(!h.children.length)h.innerHTML='<small>Ingen treff</small>'})"
    ".catch(e=>{if(e.name!='AbortError')h.innerHTML='<small>Søket trenger internett.</small>'})},300)};"
    "q.onkeydown=e=>{if(e.key=='Enter')e.preventDefault()}})</script>"
    /* Move a location up or down: swap every field with the neighbour's
     * (same name, other index), then let the departure pickers re-read theirs. */
    "<script>let L=[...document.querySelectorAll('fieldset[data-loc]')];"
    "document.querySelectorAll('.mv').forEach(b=>b.onclick=()=>{let a=b.closest('fieldset'),"
    "j=+a.dataset.loc+ +b.dataset.d,o=L[j];if(!o)return;"
    "a.querySelectorAll('input[name]').forEach(x=>{let y=o.querySelector('[name='+x.name.replace(/\\d+$/,j)+']');"
    "if(!y)return;if(x.type=='checkbox'){let c=x.checked;x.checked=y.checked;y.checked=c}"
    "else{let v=x.value;x.value=y.value;y.value=v}});"
    "[a,o].forEach(f=>f.querySelectorAll('input[name^=dep]').forEach(x=>x.dispatchEvent(new Event('change'))));"
    "o.scrollIntoView({block:'nearest',behavior:'smooth'})});</script>"
    "<script src=/dep.js></script>";

/* Build the full page into a heap buffer (caller frees). */
static char *build_page(const app_config_t *cfg)
{
    const size_t cap = 32768;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    char *p = buf;
    char *end = buf + cap;

    p += snprintf(p, end - p, "%s", PAGE_HEAD);

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
                  "<label>Tema</label><select name=theme>"
                  "<option value=0%s>Lyst</option>"
                  "<option value=1%s>M&oslash;rkt</option></select>",
                  cfg->theme == APP_THEME_DARK ? "" : " selected",
                  cfg->theme == APP_THEME_DARK ? " selected" : "");

    p += snprintf(p, end - p,
                  "<label>Natt</label><div class=chk>"
                  "<label><input type=checkbox name=dimon value=1%s>Nattmodus</label></div>"
                  "<div class=row><div><label>Fra</label>"
                  "<input name=dimstart type=time value=%02u:%02u></div>"
                  "<div><label>Til</label>"
                  "<input name=dimend type=time value=%02u:%02u></div></div>"
                  "<select name=nightoff style='margin-top:.5rem'>"
                  "<option value=0%s>Demp skjermen</option>"
                  "<option value=1%s>Sl&aring; av skjermen</option></select>"
                  "<small>Lokal tid. Dempet er skjermen fortsatt lesbar, bare m&oslash;rkere. Avsl&aring;tt "
                  "lyser den i ett minutt n&aring;r du tar p&aring; den.</small>",
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
                  "alt annet. Standard: %d og %d px, ikke fet.</small></fieldset>",
                  APP_CONFIG_TITLE_PX_MIN, APP_CONFIG_TITLE_PX_MAX, cfg->title_px,
                  cfg->title_bold ? " checked" : "",
                  APP_CONFIG_TEXT_PX_MIN, APP_CONFIG_TEXT_PX_MAX, cfg->text_px,
                  cfg->text_bold ? " checked" : "",
                  APP_CONFIG_TITLE_PX_DEFAULT, APP_CONFIG_TEXT_PX_DEFAULT);

    p += snprintf(p, end - p,
                  "<fieldset><legend>Automatisk bytte</legend>"
                  "<div class=row><div><label>Etter (min uten trykk)</label>"
                  "<input name=autoidle type=number inputmode=numeric min=0 max=%d value=%u></div>"
                  "<div><label>Per skjerm (s)</label>"
                  "<input name=autodwell type=number inputmode=numeric min=%d max=%d value=%u></div></div>"
                  "<div class=chk><label><input type=checkbox name=autoov value=1%s>Ta med oversikten</label>"
                  "<label><input type=checkbox name=autonight value=1%s>Stopp om natta</label></div>"
                  "<small>Etter s&aring; mange minutter uten trykk g&aring;r skjermen videre til neste "
                  "skjerm som er krysset av under <i>Bytt automatisk</i> nedenfor (og oversikten, "
                  "om den er tatt med), og blir st&aring;ende s&aring; lenge p&aring; hver. Et trykk stopper "
                  "det til skjermen har v&aelig;rt i fred s&aring; lenge igjen. 0 minutter sl&aring;r det "
                  "av.</small></fieldset>",
                  APP_CONFIG_AUTO_IDLE_MIN_MAX, cfg->auto_idle_min,
                  APP_CONFIG_AUTO_DWELL_S_MIN, APP_CONFIG_AUTO_DWELL_S_MAX, cfg->auto_dwell_s,
                  cfg->auto_overview ? " checked" : "", cfg->auto_night_pause ? " checked" : "");

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
    p += snprintf(p, end - p, "</fieldset>");

    /* Firmware updates (main/updater.c): the settings are saved with the
     * form; the status line and the buttons talk to /ota/status, /ota/check
     * and /ota/install (see PAGE_TAIL), which only exist once connected. */
    p += snprintf(p, end - p,
                  "<fieldset><legend>Programvareoppdatering</legend>"
                  "<div class=chk><label><input type=checkbox name=otaauto value=1%s>"
                  "Installer ny programvare automatisk</label></div>"
                  "<label>Oppdateringsadresse</label><input name=otaurl type=url autocomplete=off value=\"",
                  cfg->ota_auto ? " checked" : "");
    p = html_escape_append(p, end, cfg->ota_url);
    p += snprintf(p, end - p,
                  "\"><small>manifest.json p&aring; oppdateringssiden; manifest-test.json der for "
                  "testversjoner. Ny programvare installeres om natta, mellom 03:30 og 05:00. Bare "
                  "programvare signert med prosjektets n&oslash;kkel godtas.</small>"
                  "<p id=otast style='margin:.6rem 0 0'><small>Sjekker...</small></p><div id=otanew></div>"
                  "<div class=row><div><button type=button id=otachk>Sjekk n&aring;</button></div>"
                  "<div><button type=button id=otains hidden>Installer n&aring;</button></div></div></fieldset>");

    p += snprintf(p, end - p,
                  "<p style='margin:1.4rem 0 .2rem'><small>Ett eller flere "
                  "steder. Skjermen viser ett om gangen; trykk p&aring; h&oslash;yre "
                  "halvdel for neste, venstre for forrige, i rekkef&oslash;lgen "
                  "nedenfor (&#9650;/&#9660; flytter et sted). La en blokk st&aring; "
                  "tom for &aring; hoppe over den. Finn stedet med s&oslash;ket, eller "
                  "skriv inn breddegrad og lengdegrad. For hvert sted velger du "
                  "v&aelig;r, flyradar, skipstrafikk, nedb&oslash;rsradar og avganger "
                  "(vist i den rekkef&oslash;lgen), hvor langt radarene ser, og "
                  "korteste skip som vises: kortere skip, og skip som ikke "
                  "oppgir lengde, skjules (0 viser alle). Innenfor den valgfrie "
                  "indre sonen (0 km = ingen) gjelder en egen korteste lengde, "
                  "f.eks. alle b&aring;ter n&aelig;r, men bare store skip lenger ute. "
                  "Avganger viser kollektivtrafikk i sanntid (Entur): s&oslash;k "
                  "etter holdeplasser under feltet og kryss av linjer (ingen = "
                  "alle) og retninger (ingen = begge). S&oslash;kene trenger "
                  "internett, s&aring; de virker ikke p&aring; skjermens eget "
                  "oppsettnett. Feltet inneholder resultatet som "
                  "<code>holdeplass=linje,linje/ut;holdeplass</code> og kan "
                  "redigeres for h&aring;nd.</small>");

    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        bool filled = (i < cfg->location_count);
        p += snprintf(p, end - p,
                      "<fieldset data-loc=%d><legend>Sted %d"
                      "<button type=button class=mv data-d=-1 aria-label='Flytt opp'%s>&#9650;</button>"
                      "<button type=button class=mv data-d=1 aria-label='Flytt ned'%s>&#9660;</button></legend>"
                      "<label>Finn sted</label><input class=plq type=search autocomplete=off "
                      "placeholder='S&oslash;k etter sted, f.eks. Nittedal'><div class=hits></div>"
                      "<label>Navn</label><input name=name%d value=\"",
                      i, i + 1, i == 0 ? " disabled" : "", i == APP_CONFIG_MAX_LOCATIONS - 1 ? " disabled" : "", i);
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
        int show = filled ? cfg->show[i] : APP_SHOW_WEATHER;
        int auto_show = filled ? cfg->auto_show[i] : 0;
        int km = filled ? cfg->radar_km[i] : APP_CONFIG_RADAR_KM_DEFAULT;
        int ship_km = filled ? cfg->ship_km[i] : APP_CONFIG_SHIP_KM_DEFAULT;
        int min_len = filled ? cfg->ship_min_len_m[i] : 0;
        int near_km = filled ? cfg->ship_near_km[i] : 0;
        int near_len = filled ? cfg->ship_near_min_len_m[i] : 0;
        int rain_km = filled ? cfg->rain_km[i] : APP_CONFIG_RAIN_KM_DEFAULT;
        p += snprintf(p, end - p, "\"></div></div>"
                      "<label>Vis</label><div class=chk>"
                      "<label><input type=checkbox name=wx%d value=1%s>V&aelig;r</label>"
                      "<label><input type=checkbox name=ac%d value=1%s>Fly</label>"
                      "<label><input type=checkbox name=sh%d value=1%s>Skip</label>"
                      "<label><input type=checkbox name=rn%d value=1%s>Nedb&oslash;r</label>"
                      "<label><input type=checkbox name=dp%d value=1%s>Avganger</label></div>"
                      "<label>Bytt automatisk</label><div class=chk>"
                      "<label><input type=checkbox name=aw%d value=1%s>V&aelig;r</label>"
                      "<label><input type=checkbox name=aa%d value=1%s>Fly</label>"
                      "<label><input type=checkbox name=as%d value=1%s>Skip</label>"
                      "<label><input type=checkbox name=ar%d value=1%s>Nedb&oslash;r</label>"
                      "<label><input type=checkbox name=ad%d value=1%s>Avganger</label></div>"
                      "<div class=row><div><label>Flyradar (km)</label>"
                      "<input name=radarkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>"
                      "<div><label>Nedb&oslash;rsradar (km)</label>"
                      "<input name=rainkm%d type=number inputmode=numeric min=%d max=%d value=%d></div></div>"
                      "<div class=row><div><label>Skipstrafikk (km)</label>"
                      "<input name=shipkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>"
                      "<div><label>Korteste skip (m)</label>"
                      "<input name=shipminlen%d type=number inputmode=numeric min=0 max=%d value=%d></div></div>"
                      "<div class=row><div><label>Indre sone (km)</label>"
                      "<input name=shipnearkm%d type=number inputmode=numeric min=0 max=%d value=%d></div>"
                      "<div><label>Korteste innenfor (m)</label>"
                      "<input name=shipnearlen%d type=number inputmode=numeric min=0 max=%d value=%d></div></div>"
                      "<label>Avganger (holdeplasser og linjer)</label>"
                      "<input name=dep%d autocomplete=off spellcheck=false maxlength=%d value=\"",
                      i, (show & APP_SHOW_WEATHER) ? " checked" : "",
                      i, (show & APP_SHOW_RADAR) ? " checked" : "",
                      i, (show & APP_SHOW_SHIPS) ? " checked" : "",
                      i, (show & APP_SHOW_RAIN) ? " checked" : "",
                      i, (show & APP_SHOW_DEPARTURES) ? " checked" : "",
                      i, (auto_show & APP_SHOW_WEATHER) ? " checked" : "",
                      i, (auto_show & APP_SHOW_RADAR) ? " checked" : "",
                      i, (auto_show & APP_SHOW_SHIPS) ? " checked" : "",
                      i, (auto_show & APP_SHOW_RAIN) ? " checked" : "",
                      i, (auto_show & APP_SHOW_DEPARTURES) ? " checked" : "",
                      i, APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX, km,
                      i, APP_CONFIG_RAIN_KM_MIN, APP_CONFIG_RAIN_KM_MAX, rain_km,
                      i, APP_CONFIG_SHIP_KM_MIN, APP_CONFIG_SHIP_KM_MAX, ship_km,
                      i, APP_CONFIG_SHIP_MIN_LEN_MAX, min_len,
                      i, APP_CONFIG_SHIP_NEAR_KM_MAX, near_km,
                      i, APP_CONFIG_SHIP_MIN_LEN_MAX, near_len,
                      i, APP_CONFIG_DEPARTURES_MAX - 1);
        if (filled) {
            p = html_escape_append(p, end, cfg->departures[i]);
        }
        p += snprintf(p, end - p, "\"></fieldset>");
    }

    p += snprintf(p, end - p, "%s", PAGE_TAIL);
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

#define SAVE_BODY_MAX 8192

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
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* "HH:MM" (as an <input type=time> sends it) to minutes after midnight, or
 * -1 if it isn't a valid time. */
static int parse_hhmm(const char *s)
{
    int h, m;
    char extra;
    if (sscanf(s, "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
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
        cfg->auto_night_pause = form_field(body, "autonight", val, sizeof(val)) ? 1 : 0;
    }
    /* Firmware updates: the address is always sent, the checkbox only when
     * ticked. Blank turns checking off; anything but http(s) is refused. */
    char url[APP_CONFIG_OTA_URL_MAX];
    if (form_field(body, "otaurl", url, sizeof(url))) {
        char val[4];
        cfg->ota_auto = form_field(body, "otaauto", val, sizeof(val)) ? 1 : 0;
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
    char *dep_raw = malloc(3 * APP_CONFIG_DEPARTURES_MAX);
    if (deps == NULL || dep_raw == NULL) {
        free(deps);
        free(dep_raw);
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
            free(dep_raw);
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
        /* form_field() truncates before URL-decoding, and the IDs' ':' come
         * in as "%3A": read the raw field into a buffer three times the size. */
        snprintf(key, sizeof(key), "dep%d", i);
        form_field(body, key, dep_raw, 3 * APP_CONFIG_DEPARTURES_MAX);
        snprintf(deps[n], APP_CONFIG_DEPARTURES_MAX, "%s", dep_raw);
        show[n] = sh ? sh : APP_SHOW_WEATHER;
        static const struct { const char *key; uint8_t bit; } rot[] = {
            { "aw%d", APP_SHOW_WEATHER }, { "aa%d", APP_SHOW_RADAR }, { "as%d", APP_SHOW_SHIPS },
            { "ar%d", APP_SHOW_RAIN }, { "ad%d", APP_SHOW_DEPARTURES },
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
    free(dep_raw);

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
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
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
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* The departure picker script (components/setup_departures.js, embedded). */
static esp_err_t h_dep_js(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    extern const char dep_js_start[] asm("_binary_setup_departures_js_start");
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, dep_js_start);
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
    config.max_uri_handlers = 20;
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
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
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
    esp_netif_create_default_wifi_sta();
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

    xTaskCreate(dns_task, "captdns", 3072, NULL, 4, NULL);
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
            esp_restart();
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

/* Announce the setup page as http://multidisplay.local/ on the home
 * network. */
static void mdns_announce(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS didn't start");
        return;
    }
    mdns_hostname_set("multidisplay");
    mdns_instance_name_set("MultiDisplay");
    mdns_service_add("MultiDisplay", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "Announced as multidisplay.local");
}

esp_err_t wifi_provision_connect(const app_config_t *cfg, wifi_provision_status_fn status_fn)
{
    s_status = status_fn;

    net_common_init();

    bool force_portal = boot_button_held();
    snprintf(s_web_pass, sizeof(s_web_pass), "%s", cfg->web_pass);
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
