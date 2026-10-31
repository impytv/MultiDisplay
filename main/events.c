/* The display's own events (components/own_events.c): a copy in memory for
 * the screens - the calendar, the month calendar and the clock ask for the
 * days in their window (events_days) and redraw when the generation
 * changes - and the page /hendelser where they are typed in.
 *
 * The page lists them by the next day each is on, passed ones last, with a
 * form above to add one or change one (/hendelser?endre=N). Saving writes
 * NVS at once; no restart. An edit or delete names the event by its index
 * and its text, so a list changed in another browser meanwhile is refused
 * rather than the wrong one changed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app.h"
#include "events.h"
#include "form_util.h"
#include "own_events.h"
#include "wifi_provision.h"

static const char *TAG = "events";

#define PAGE_MAX (96 * 1024) /* PSRAM; a full list is about 50 KB */
#define BODY_MAX 1024
#define PRUNE_DAYS 30 /* a passed one-date event is deleted this long after, on adding one */

static SemaphoreHandle_t s_lock;
static EXT_RAM_BSS_ATTR own_event_t s_ev[OWN_MAX];
static int s_n;
static volatile uint32_t s_gen = 1;

static const char *const MONTH[] = { "januar", "februar", "mars", "april", "mai", "juni", "juli",
                                     "august", "september", "oktober", "november", "desember" };

void events_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_n = own_load(s_ev);
    ESP_LOGI(TAG, "%d event(s)", s_n);
}

uint32_t events_generation(void)
{
    return s_gen;
}

int events_count(void)
{
    return s_n;
}

int events_days(int64_t from, int64_t to, uint8_t where, cal_list_t *out)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = own_days(s_ev, s_n, from, to, where, out);
    xSemaphoreGive(s_lock);
    return n;
}

/* --------------------------------------------------------------------------
 * The page
 * ------------------------------------------------------------------------ */

static char *esc(char *p, char *end, const char *s)
{
    for (; *s && p < end - 7; s++) {
        switch (*s) {
        case '"': p += sprintf(p, "&quot;"); break;
        case '&': p += sprintf(p, "&amp;"); break;
        case '<': p += sprintf(p, "&lt;"); break;
        case '>': p += sprintf(p, "&gt;"); break;
        case '\'': p += sprintf(p, "&#39;"); break;
        default: *p++ = *s; break;
        }
    }
    *p = '\0';
    return p;
}

static int64_t today_midnight(void)
{
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt);
}

/* "Vises p&aring; kalenderen, m&aring;nedskalenderen og klokken". */
static char *where_text(char *p, char *end, uint8_t show)
{
    static const char *const NAME[] = { "kalenderen", "m&aring;nedskalenderen", "klokken" };
    int n = 0, total = 0;
    for (int b = 0; b < 3; b++) {
        total += (show >> b) & 1;
    }
    p = buf_append(p, end, "Vises p&aring; ");
    for (int b = 0; b < 3; b++) {
        if (show & (1 << b)) {
            n++;
            p = buf_append(p, end, "%s%s", n == 1 ? "" : (n == total ? " og " : ", "), NAME[b]);
        }
    }
    return p;
}

/* What the form shows: an event being added or changed (idx), and why the
 * last try was refused. */
typedef struct {
    own_event_t e;
    int idx; /* -1: a new one */
    char was[OWN_TEXT_MAX];
    const char *err;
} form_t;

static char *build_form(char *p, char *end, const form_t *f)
{
    const bool edit = f->idx >= 0;
    p = buf_append(p, end, "<form method=post action=/hendelser><fieldset><legend>%s</legend>",
                   edit ? "Endre hendelsen" : "Ny hendelse");
    if (f->err != NULL) {
        p = buf_append(p, end, "<p class=err>%s</p>", f->err);
    }
    p = buf_append(p, end, "<input type=hidden name=i value=%d><input type=hidden name=was value=\"", f->idx);
    p = esc(p, end, f->was);
    p = buf_append(p, end, "\"><label for=kind>Type</label><select name=kind id=kind>");
    for (int k = 0; k < OWN_KINDS; k++) {
        p = buf_append(p, end, "<option value=%d%s>%s</option>", k, f->e.kind == k ? " selected" : "",
                       own_kind_name(k));
    }
    p = buf_append(p, end, "</select><label for=text id=tl>Hva er det?</label>"
                           "<input name=text id=text maxlength=40 required value=\"");
    p = esc(p, end, f->e.text);
    p = buf_append(p, end, "\"><div class=row><div><label for=day>Dag</label><select name=day id=day>");
    for (int d = 1; d <= 31; d++) {
        p = buf_append(p, end, "<option%s>%d</option>", f->e.day == d ? " selected" : "", d);
    }
    p = buf_append(p, end, "</select></div><div><label for=month>M&aring;ned</label><select name=month id=month>");
    for (int m = 1; m <= 12; m++) {
        p = buf_append(p, end, "<option value=%d%s>%s</option>", m, f->e.month == m ? " selected" : "", MONTH[m - 1]);
    }
    p = buf_append(p, end, "</select></div><div><label for=year id=yl>&Aring;r</label>"
                           "<input name=year id=year inputmode=numeric maxlength=4 autocomplete=off value=\"");
    if (f->e.year != 0) {
        p = buf_append(p, end, "%d", f->e.year);
    }
    p = buf_append(p, end, "\"></div></div><small id=yh></small><label>Vises p&aring;</label><div class=chk>");
    static const char *const NAME[] = { "Kalender", "M&aring;nedskalender", "Klokken" };
    static const char *const KEY[] = { "cal", "mon", "clk" };
    for (int b = 0; b < 3; b++) {
        p = buf_append(p, end, "<label><input type=checkbox name=%s value=1%s>%s</label>", KEY[b],
                       f->e.show & (1 << b) ? " checked" : "", NAME[b]);
    }
    p = buf_append(p, end, "</div><button type=submit>%s</button>%s</fieldset></form>",
                   edit ? "Lagre endringen" : "Legg til",
                   edit ? "<p><a href=/hendelser>Avbryt</a></p>" : "");
    return p;
}

/* One event in the list. */
static char *build_item(char *p, char *end, int i, int64_t next, int64_t today)
{
    const own_event_t *e = &s_ev[i];
    char title[CAL_TITLE_MAX];
    int year = e->year;
    if (next != 0) {
        const time_t t = (time_t)next;
        struct tm lt;
        localtime_r(&t, &lt);
        year = lt.tm_year + 1900;
    }
    own_title(e, year, title, sizeof(title));
    p = buf_append(p, end, "<div class=\"ev%s\"><span class=dot style=\"background:#%06lx\"></span><div class=evt><b>",
                   next == 0 ? " past" : "", (unsigned long)own_kind_colour(e->kind));
    p = esc(p, end, title);
    p = buf_append(p, end, "</b><small>%s &middot; %d. %s", own_kind_name(e->kind), e->day, MONTH[e->month - 1]);
    if (e->kind == OWN_BIRTHDAY && e->year != 0) {
        p = buf_append(p, end, " (f&oslash;dt %d)", e->year);
    } else if (e->kind == OWN_ANNIVERSARY && e->year != 0) {
        p = buf_append(p, end, " (fra %d)", e->year);
    } else if (e->year != 0) {
        p = buf_append(p, end, " %d", e->year);
    }
    if (next == 0) {
        p = buf_append(p, end, " &middot; passert");
    } else if (today > 0) {
        const int days = (int)((next - today + 43200) / 86400);
        if (days == 0) {
            p = buf_append(p, end, " &middot; <b>i dag</b>");
        } else if (days == 1) {
            p = buf_append(p, end, " &middot; i morgen");
        } else {
            p = buf_append(p, end, " &middot; om %d dager", days);
        }
    }
    p = buf_append(p, end, "<br>");
    p = where_text(p, end, e->show);
    p = buf_append(p, end, "</small></div><div class=evb><a href=\"/hendelser?endre=%d\">Endre</a>"
                           "<form method=post action=/hendelser/slett onsubmit=\"return confirm('Slette '+this.dataset.t+'?')\" "
                           "data-t=\"", i);
    p = esc(p, end, title);
    p = buf_append(p, end, "\"><input type=hidden name=i value=%d><input type=hidden name=was value=\"", i);
    p = esc(p, end, e->text);
    return buf_append(p, end, "\"><button type=submit>Slett</button></form></div></div>");
}

/* By the next day each is on; passed ones last, the latest first. */
static EXT_RAM_BSS_ATTR int64_t s_sort_next[OWN_MAX];

static int cmp_item(const void *a, const void *b)
{
    const int i = *(const int *)a, j = *(const int *)b;
    const int64_t x = s_sort_next[i], y = s_sort_next[j];
    if ((x == 0) != (y == 0)) {
        return x == 0 ? 1 : -1;
    }
    if (x == 0) {
        const int kx = s_ev[i].year * 400 + s_ev[i].month * 32 + s_ev[i].day;
        const int ky = s_ev[j].year * 400 + s_ev[j].month * 32 + s_ev[j].day;
        return ky - kx;
    }
    return x < y ? -1 : x > y ? 1 : i - j;
}

/* What the kind changes on the form: the labels, whether the year is
 * needed, and the hint under it. */
static const char FORM_SCRIPT[] =
    "<script>(function(){var k=document.getElementById('kind'),t=document.getElementById('tl'),"
    "y=document.getElementById('yl'),v=document.getElementById('year'),h=document.getElementById('yh');"
    "var T=[['Hvem har bursdag?','F\xC3\xB8""dsels\xC3\xA5r','valgfritt','Med f\xC3\xB8""dsels\xC3\xA5ret st\xC3\xA5r alderen p\xC3\xA5 skjermen: "
    "\xC2\xABKari fyller 80\xC2\xBB. Vises hvert \xC3\xA5r.'],['Hva er merkedagen?','Fra \xC3\xA5r','valgfritt','Med \xC3\xA5ret st\xC3\xA5r det hvor "
    "mange \xC3\xA5r det er: \xC2\xAB""Bryllupsdag (25 \xC3\xA5r)\xC2\xBB. Vises hvert \xC3\xA5r.'],['Hva skjer?','\xC3\x85r','','Vises bare denne "
    "datoen.'],['Hva skjer?','\xC3\x85r','','Vises bare denne datoen.']];"
    "function u(){var a=T[k.value];t.textContent=a[0];y.textContent=a[1];v.placeholder=a[2];"
    "v.required=k.value>1;h.textContent=a[3];if(k.value>1&&!v.value)v.value=new Date().getFullYear();}"
    "k.onchange=u;u();})();</script>";

static const char PAGE_STYLE[] =
    "<style>.ev{display:flex;gap:.6rem;align-items:flex-start;padding:.6rem 0;border-bottom:1px solid var(--line)}"
    ".ev .dot{flex:none;width:.8rem;height:.8rem;border-radius:50%;margin-top:.35rem}"
    ".evt{flex:1;min-width:0}.evt small{display:block;margin-top:.15rem}"
    ".evb{display:flex;flex-direction:column;align-items:flex-end;gap:.3rem}.evb form{margin:0}"
    ".evb button{margin:0;padding:0;width:auto;background:none;color:var(--err);font-size:.9rem}"
    ".evb a{font-size:.9rem}.ev.past{opacity:.6}p.err{color:var(--err);margin:.4rem 0}"
    "p.ok{color:var(--acc);font-weight:600}</style>";

static esp_err_t send_page(httpd_req_t *req, const form_t *f, const char *ok)
{
    char *buf = malloc(PAGE_MAX);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }
    char *p = buf, *end = buf + PAGE_MAX;
    p = buf_append(p, end, "%s%s<title>Bursdager og hendelser - V&aelig;rtavla</title>"
                           "<h1>Bursdager og hendelser</h1><p><a href=/>&larr; Navigasjon</a> &middot; "
                           "<a href=/oppsett>Oppsett</a></p>",
                   wifi_provision_page_head(), PAGE_STYLE);
    if (ok != NULL) {
        p = buf_append(p, end, "<p class=ok>%s</p>", ok);
    }
    p = build_form(p, end, f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const time_t now = time(NULL);
    const int64_t today = now > PLAUSIBLE_EPOCH_S ? today_midnight() : 0;
    static EXT_RAM_BSS_ATTR int order[OWN_MAX];
    for (int i = 0; i < s_n; i++) {
        order[i] = i;
        s_sort_next[i] = own_next(&s_ev[i], today > 0 ? today : (int64_t)now);
    }
    qsort(order, (size_t)s_n, sizeof(order[0]), cmp_item);
    p = buf_append(p, end, "<h2>Lagt inn</h2><small>%d av %d. Bursdager og merkedager vises hvert &aring;r, "
                           "avtaler og annet bare den ene datoen. N&aring;r du legger til en ny, "
                           "slettes avtaler og annet som var for mer enn %d dager siden.</small>", s_n, OWN_MAX, PRUNE_DAYS);
    if (s_n == 0) {
        p = buf_append(p, end, "<p><small>Ingen hendelser enn&aring;.</small></p>");
    }
    for (int k = 0; k < s_n; k++) {
        p = build_item(p, end, order[k], s_sort_next[order[k]], today);
    }
    xSemaphoreGive(s_lock);
    buf_append(p, end, "%s", FORM_SCRIPT);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static void new_form(form_t *f)
{
    memset(f, 0, sizeof(*f));
    f->idx = -1;
    f->e.kind = OWN_BIRTHDAY;
    f->e.show = OWN_ON_ALL;
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    f->e.day = (uint8_t)lt.tm_mday;
    f->e.month = (uint8_t)(lt.tm_mon + 1);
}

/* GET /hendelser, or /hendelser?endre=N with event N in the form. */
static esp_err_t h_page(httpd_req_t *req)
{
    static EXT_RAM_BSS_ATTR form_t f; /* the server's one task */
    new_form(&f);
    char q[48], v[8];
    const char *ok = NULL;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "endre", v, sizeof(v)) == ESP_OK) {
            const int i = atoi(v);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (i >= 0 && i < s_n) {
                f.e = s_ev[i];
                f.idx = i;
                snprintf(f.was, sizeof(f.was), "%s", s_ev[i].text);
            }
            xSemaphoreGive(s_lock);
        }
        if (httpd_query_key_value(q, "ok", v, sizeof(v)) == ESP_OK) {
            ok = v[0] == '1' ? "Lagt til." : v[0] == '2' ? "Endret." : v[0] == '3' ? "Slettet." : NULL;
        }
    }
    return send_page(req, &f, ok);
}

static bool read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        return false;
    }
    size_t total = 0;
    while (total < req->content_len) {
        const int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            return false;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    return true;
}

static esp_err_t see_other(httpd_req_t *req, const char *where)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", where);
    return httpd_resp_send(req, NULL, 0);
}

/* Whether event i is still the one the form was for. Lock held. */
static bool same_event(int i, const char *was)
{
    return i >= 0 && i < s_n && strcmp(s_ev[i].text, was) == 0;
}

/* Save `n` events from `ev` and make them the ones shown. Lock held. */
static esp_err_t commit(const own_event_t *ev, int n)
{
    esp_err_t err = own_save(ev, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Events not saved: %s", esp_err_to_name(err));
        return err;
    }
    if (ev != s_ev) {
        memmove(s_ev, ev, (size_t)n * sizeof(own_event_t));
    }
    s_n = n;
    s_gen++;
    return ESP_OK;
}

static const char ERR_CHANGED[] =
    "Hendelsen er endret eller slettet et annet sted i mellomtiden. Se listen under.";

/* POST /hendelser: add one (i = -1) or change event i. */
static esp_err_t h_save(httpd_req_t *req)
{
    static EXT_RAM_BSS_ATTR char body[BODY_MAX];
    static EXT_RAM_BSS_ATTR form_t f;
    new_form(&f);
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "413 Content Too Large");
        return httpd_resp_sendstr(req, "Skjemaet er for stort.");
    }
    char v[12];
    f.idx = form_field(body, "i", v, sizeof(v)) ? atoi(v) : -1;
    form_field(body, "was", f.was, sizeof(f.was));
    f.e.kind = (uint8_t)(form_field(body, "kind", v, sizeof(v)) ? atoi(v) : 0);
    form_field(body, "text", f.e.text, sizeof(f.e.text));
    /* Spaces round it are a slip. */
    char *t = f.e.text;
    size_t len = strlen(t);
    while (len > 0 && t[len - 1] == ' ') {
        t[--len] = '\0';
    }
    const size_t lead = strspn(t, " ");
    memmove(t, t + lead, strlen(t + lead) + 1);
    f.e.day = (uint8_t)(form_field(body, "day", v, sizeof(v)) ? atoi(v) : 0);
    f.e.month = (uint8_t)(form_field(body, "month", v, sizeof(v)) ? atoi(v) : 0);
    bool year_ok = true;
    f.e.year = 0;
    if (form_field(body, "year", v, sizeof(v)) && v[0] != '\0') {
        char *e;
        const long y = strtol(v, &e, 10);
        year_ok = *e == '\0' && y >= 1900 && y <= 2200;
        f.e.year = year_ok ? (uint16_t)y : 0;
    }
    f.e.show = (form_field(body, "cal", v, sizeof(v)) ? OWN_ON_CALENDAR : 0) |
               (form_field(body, "mon", v, sizeof(v)) ? OWN_ON_MONTH : 0) |
               (form_field(body, "clk", v, sizeof(v)) ? OWN_ON_CLOCK : 0);

    const bool every_year = f.e.kind == OWN_BIRTHDAY || f.e.kind == OWN_ANNIVERSARY;
    if (f.e.kind >= OWN_KINDS) {
        f.e.kind = OWN_OTHER;
    }
    if (f.e.text[0] == '\0') {
        f.err = "Skriv hva det er.";
    } else if (!year_ok) {
        f.err = "&Aring;ret m&aring; v&aelig;re mellom 1900 og 2200.";
    } else if (!every_year && f.e.year == 0) {
        f.err = "Skriv &aring;ret.";
    } else if (f.e.show == 0) {
        f.err = "Kryss av for minst &eacute;n skjerm den skal vises p&aring;.";
    } else if (!own_valid(&f.e)) {
        f.err = "Den datoen finnes ikke.";
    }
    if (f.err != NULL) {
        return send_page(req, &f, NULL);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (f.idx >= 0 && !same_event(f.idx, f.was)) {
        f.err = ERR_CHANGED;
        f.idx = -1;
    } else {
        static EXT_RAM_BSS_ATTR own_event_t ev[OWN_MAX];
        memcpy(ev, s_ev, (size_t)s_n * sizeof(own_event_t));
        int n = s_n;
        if (f.idx < 0 && time(NULL) > PLAUSIBLE_EPOCH_S) {
            /* Only on adding, so the passed ones stay until there's a new one. */
            n = own_prune(ev, n, today_midnight(), PRUNE_DAYS);
            if (n != s_n) {
                ESP_LOGI(TAG, "%d passed event(s) deleted", s_n - n);
            }
        }
        if (f.idx < 0 && n >= OWN_MAX) {
            f.err = "Det er ikke plass til flere. Slett noen du ikke trenger.";
        } else {
            ev[f.idx >= 0 ? f.idx : n++] = f.e;
            err = commit(ev, n);
            if (err != ESP_OK) {
                f.err = "Kunne ikke lagre. Pr&oslash;v igjen.";
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (f.err != NULL) {
        return send_page(req, &f, NULL);
    }
    ESP_LOGI(TAG, "%s an event", f.idx >= 0 ? "Changed" : "Added");
    app_wake_poll();
    return see_other(req, f.idx >= 0 ? "/hendelser?ok=2" : "/hendelser?ok=1");
}

/* POST /hendelser/slett: delete event i. */
static esp_err_t h_delete(httpd_req_t *req)
{
    char body[160], v[12], was[OWN_TEXT_MAX];
    if (!read_body(req, body, sizeof(body)) || !form_field(body, "i", v, sizeof(v))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Mangler hvilken hendelse.");
    }
    form_field(body, "was", was, sizeof(was));
    const int i = atoi(v);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *why = NULL;
    if (!same_event(i, was)) {
        why = ERR_CHANGED;
    } else {
        static EXT_RAM_BSS_ATTR own_event_t ev[OWN_MAX];
        memcpy(ev, s_ev, (size_t)s_n * sizeof(own_event_t));
        memmove(&ev[i], &ev[i + 1], (size_t)(s_n - i - 1) * sizeof(own_event_t));
        if (commit(ev, s_n - 1) != ESP_OK) {
            why = "Kunne ikke slette. Pr&oslash;v igjen.";
        }
    }
    xSemaphoreGive(s_lock);
    if (why != NULL) {
        static EXT_RAM_BSS_ATTR form_t f;
        new_form(&f);
        f.err = why;
        return send_page(req, &f, NULL);
    }
    ESP_LOGI(TAG, "Deleted an event");
    app_wake_poll();
    return see_other(req, "/hendelser?ok=3");
}

void events_add_handlers(void)
{
    wifi_provision_add_page_handler("/hendelser", h_page);
    wifi_provision_add_post_handler("/hendelser", h_save);
    wifi_provision_add_post_handler("/hendelser/slett", h_delete);
}
