/* The picture screen: JPEG pictures from the microSD card (sd_card.c),
 * one at a time in random order (pic_util.c's bag: each once before any
 * comes again), each for g_cfg->pic_secs, centred on black.
 *
 * Pictures come from the /bilder page (pictures_page.js), which shrinks
 * them in the browser to fit 800x480 before sending, so they are 100-250
 * KB each; a picture from an address is fetched by the display for the
 * page (GET /bilder/hent - other sites' pictures can't be read by a page
 * of the display's) and shrunk the same way. JPEGs copied onto the card's
 * BILDER folder from a PC are shown too, decoded at up to 1/8 of their
 * size if bigger than the screen, if the file is at most PIC_FILE_MAX.
 *
 * Memory: one 800x480 RGB565 buffer (750 KB of PSRAM) while the screen is
 * on show, given back on any other (pictures_release), and the file being
 * decoded for a moment. A picture is decoded straight into that buffer
 * with the adapter lock held, so a half-decoded picture is never drawn. */

#include "pictures.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_random.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "form_util.h"
#include "http_util.h"
#include "pic_util.h"
#include "sd_card.h"
#include "watchdog.h"
#include "wifi_provision.h"

static const char *TAG = "pictures";

#define PIC_W 800
#define PIC_H 480
#define PIC_BYTES (PIC_W * PIC_H * 2)
#define PIC_DIR SD_CARD_ROOT "/BILDER"
#define PIC_PATH_MAX (sizeof(PIC_DIR) + PIC_NAME_MAX + 8) /* "/sd/BILDER/" and a name */
#define PIC_MAX 1000                    /* pictures listed */
#define PIC_FILE_MAX (1536 * 1024)      /* a JPEG copied on from a PC, at most */
#define PIC_UPLOAD_MAX (1024 * 1024)    /* one from the page (they are ~200 KB) */
/* Left beside a file being read or received (for the moment until it is
 * decoded or saved): this much PSRAM in all, and a block this big, for
 * what the other tasks fetch meanwhile (TLS, replies of some tens of KB).
 * With the screen's 750 KB picture taken, the largest block left is often
 * under 500 KB, so asking for more in one piece kept every picture but
 * the smallest from showing (1.37.0, before release). */
#define PIC_PSRAM_LEFT (256 * 1024)
#define PIC_PSRAM_BLOCK_LEFT (128 * 1024)
#define PIC_CARD_RETRY_MS 10000         /* no card, or nothing on it: look again */
#define PIC_FETCH_MAX (20 * 1024 * 1024) /* a picture fetched for the page */
#define PIC_FETCH_TIMEOUT_MS 20000
#define PIC_FETCH_STACK 12288

/* Bumped when the page adds or deletes a picture, so the screen lists the
 * card again. */
static volatile uint32_t s_gen = 1;

/* ---- The screen (weather task, and the LVGL task under the lock) ---- */

static lv_obj_t *s_root;
static lv_obj_t *s_img;
static lv_image_dsc_t s_dsc;
static uint16_t *s_px;   /* PIC_BYTES, while on show */
static bool s_shown;     /* s_px holds a picture */
static uint32_t s_shown_tick;
static uint32_t s_list_gen; /* s_gen when listed, 0 = not yet */
/* The card's pictures and their order: PSRAM while on show, like s_px. */
typedef struct {
    char names[PIC_MAX][PIC_NAME_MAX];
    uint16_t order[PIC_MAX];
} pic_list_t;
static pic_list_t *s_list;
#define s_names (s_list->names)
static pic_bag_t s_bag = { .cap = PIC_MAX };
static int s_count;
static char s_on_show[PIC_NAME_MAX]; /* the picture in s_px */
static const char *s_note;           /* the status text, NULL = none */

static void show_note(void)
{
    lv_label_set_text(g_status_label, s_note ? s_note : "");
    if (s_shown) {
        lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *pictures_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_img = lv_image_create(s_root);
    lv_obj_center(s_img);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    return s_root;
}

void pictures_enter(void)
{
    if (!s_shown && s_note == NULL) {
        lv_label_set_text(g_status_label, "Henter bildene...");
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    show_note();
}

void pictures_release(void)
{
    if (s_px == NULL || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_img, NULL);
    lv_image_cache_drop(&s_dsc);
    uint16_t *px = s_px;
    s_px = NULL;
    s_shown = false;
    s_on_show[0] = '\0';
    esp_lv_adapter_unlock();
    heap_caps_free(px);
    heap_caps_free(s_list); /* only this task uses it */
    s_list = NULL;
    s_bag.order = NULL;
    s_list_gen = 0;
    wd_psram_held(-(int)(PIC_BYTES + sizeof(pic_list_t)));
}

/* The card's BILDER folder into s_names (SD lock held). False if it can't
 * be read. */
static bool list_card(void)
{
    DIR *d = opendir(PIC_DIR);
    if (d == NULL) {
        if (mkdir(PIC_DIR, 0777) != 0 && errno != EEXIST) {
            return false;
        }
        d = opendir(PIC_DIR);
        if (d == NULL) {
            return false;
        }
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < PIC_MAX) {
        if (e->d_type != DT_DIR && strlen(e->d_name) < PIC_NAME_MAX && pic_is_picture(e->d_name)) {
            snprintf(s_names[n++], PIC_NAME_MAX, "%s", e->d_name);
        }
    }
    closedir(d);
    s_count = n;
    pic_bag_reset(&s_bag, n);
    /* The one on show counts as just shown, so the new order doesn't
     * start with it. */
    for (int i = 0; i < n && s_on_show[0]; i++) {
        if (strcmp(s_names[i], s_on_show) == 0) {
            s_bag.last = i;
            break;
        }
    }
    if (n == PIC_MAX) {
        ESP_LOGW(TAG, "Only the first %d pictures on the card are shown", PIC_MAX);
    }
    return true;
}

/* Whether `len` bytes can be taken from PSRAM for a moment and leave
 * PIC_PSRAM_LEFT, and a block of PIC_PSRAM_BLOCK_LEFT in what it is
 * taken from, for the rest. */
static bool psram_room(size_t len)
{
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return len + PIC_PSRAM_BLOCK_LEFT <= largest && len + PIC_PSRAM_LEFT <= free_b;
}

/* File `name` from the card into a new PSRAM buffer (SD lock held). */
static uint8_t *read_picture(const char *name, size_t *len)
{
    char path[PIC_PATH_MAX];
    snprintf(path, sizeof(path), PIC_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    uint8_t *buf = NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > PIC_FILE_MAX) {
        ESP_LOGW(TAG, "%s skipped: %ld bytes", name, (long)st.st_size);
    } else if (!psram_room((size_t)st.st_size) ||
               (buf = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM)) == NULL) {
        ESP_LOGW(TAG, "No room to read %s (%ld bytes)", name, (long)st.st_size);
    } else if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        ESP_LOGW(TAG, "Couldn't read %s", name);
        heap_caps_free(buf);
        buf = NULL;
    } else {
        *len = (size_t)st.st_size;
    }
    fclose(f);
    return buf;
}

/* The size of the JPEG in jpg, decoded at `sw` x `sh` (0 = as it is):
 * opened and its header parsed into *dec (to be closed by the caller). */
static bool jpeg_open(const uint8_t *jpg, size_t len, int sw, int sh, jpeg_dec_handle_t *dec, jpeg_dec_io_t *io,
                      jpeg_dec_header_info_t *info)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.scale.width = (uint16_t)sw;
    cfg.scale.height = (uint16_t)sh;
    *dec = NULL;
    if (jpeg_dec_open(&cfg, dec) != JPEG_ERR_OK) {
        return false;
    }
    *io = (jpeg_dec_io_t){ .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    return jpeg_dec_parse_header(*dec, io, info) == JPEG_ERR_OK;
}

/* Decode jpg into s_px, fitted to the screen, and show it. Only this task
 * writes s_px, and LVGL is told to stop drawing from it before it does,
 * so the decode itself - up to seconds for a big photo copied on from a
 * computer - holds no lock: taps and other screens go on meanwhile. A
 * picture found unusable before that leaves the one on show as it is. */
static bool decode_into_screen(int for_view, const char *name, const uint8_t *jpg, size_t len)
{
    jpeg_dec_handle_t dec;
    jpeg_dec_io_t io;
    jpeg_dec_header_info_t info = { 0 };
    bool ok = jpeg_open(jpg, len, 0, 0, &dec, &io, &info);
    const int iw = info.width, ih = info.height;
    if (dec != NULL) {
        jpeg_dec_close(dec);
    }
    int w = 0, h = 0;
    if (!ok || !pic_fit(iw, ih, PIC_W, PIC_H, &w, &h)) {
        ESP_LOGW(TAG, "%s skipped: %s (%dx%d)", name, ok ? "too large to shrink" : "not a JPEG it can read", iw, ih);
        return false;
    }
    const bool scaled = w != iw || h != ih;
    ok = jpeg_open(jpg, len, scaled ? w : 0, scaled ? h : 0, &dec, &io, &info);
    int out_len = 0;
    if (ok) {
        ok = jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK && out_len >= w * h * 2 && out_len <= PIC_BYTES;
        if (!ok) {
            ESP_LOGW(TAG, "%s skipped: %d bytes decoded at %dx%d", name, out_len, w, h);
        }
    }
    if (ok && esp_lv_adapter_lock(-1) == ESP_OK) {
        /* Not drawn from while it changes. */
        lv_image_set_src(s_img, NULL);
        lv_image_cache_drop(&s_dsc);
        s_shown = false;
        s_on_show[0] = '\0';
        esp_lv_adapter_unlock();
    } else {
        ok = false;
    }
    if (ok) {
        io.outbuf = (uint8_t *)s_px;
        ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
        if (!ok) {
            ESP_LOGW(TAG, "%s couldn't be decoded", name);
        }
    }
    if (dec != NULL) {
        jpeg_dec_close(dec);
    }
    if (!ok || esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    /* A width the decoder pads: the row length from what it gave. */
    const int stride_px = (out_len % (h * 2) == 0 && out_len / (h * 2) >= w) ? out_len / (h * 2) : w;
    s_dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = w, .h = h,
                    .stride = stride_px * 2 },
        .data = (const uint8_t *)s_px,
        .data_size = (uint32_t)(stride_px * h * 2),
    };
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_center(s_img);
    s_shown = true;
    s_note = NULL;
    snprintf(s_on_show, sizeof(s_on_show), "%s", name);
    if (g_view_index == for_view) {
        show_note();
    }
    esp_lv_adapter_unlock();
    return true;
}

static uint32_t random32(void)
{
    return esp_random();
}

/* Note `note` (NULL = none) for the screen if it is still on show. */
static void set_note(int for_view, const char *note)
{
    if (lock_for_view(for_view)) {
        s_note = note;
        show_note();
        esp_lv_adapter_unlock();
    } else {
        s_note = note;
    }
}

uint32_t pictures_poll(int for_view)
{
    if (s_px == NULL) {
        uint16_t *px = heap_caps_aligned_alloc(16, PIC_BYTES, MALLOC_CAP_SPIRAM);
        pic_list_t *list = heap_caps_malloc(sizeof(pic_list_t), MALLOC_CAP_SPIRAM);
        if (px == NULL || list == NULL) {
            ESP_LOGE(TAG, "Out of memory for the picture (%u bytes free in one piece)",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            heap_caps_free(px);
            heap_caps_free(list);
            set_note(for_view, "Ikke nok minne til bildet.");
            return PIC_CARD_RETRY_MS;
        }
        s_list = list;
        s_bag.order = list->order;
        s_list_gen = 0;
        wd_psram_held((int)(PIC_BYTES + sizeof(pic_list_t)));
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            s_px = px;
            s_shown = false;
            esp_lv_adapter_unlock();
        }
    }

    sd_card_lock();
    if (sd_card_ready() != ESP_OK) {
        const bool unreadable = sd_card_unreadable() != NULL;
        sd_card_unlock();
        s_list_gen = 0;
        if (lock_for_view(for_view)) {
            s_shown = false;
            s_note = unreadable ? "Minnekortet kan ikke leses. Formater det som exFAT eller FAT32."
                                : "Fant ikke noe minnekort. Sett inn et microSD-kort med bilder.";
            show_note();
            esp_lv_adapter_unlock();
        }
        return PIC_CARD_RETRY_MS;
    }
    const uint32_t gen = s_gen;
    bool force = false;
    if (s_list_gen != gen) {
        if (!list_card()) {
            sd_card_failed();
            sd_card_unlock();
            s_list_gen = 0;
            set_note(for_view, "Kan ikke lese minnekortet.");
            return PIC_CARD_RETRY_MS;
        }
        s_list_gen = gen;
        /* The one on show deleted meanwhile: on to the next at once. */
        force = true;
        for (int i = 0; i < s_count && s_on_show[0]; i++) {
            force &= strcmp(s_names[i], s_on_show) != 0;
        }
        force |= !s_on_show[0];
    }
    if (s_count == 0) {
        sd_card_unlock();
        if (lock_for_view(for_view)) {
            s_shown = false;
            s_on_show[0] = '\0';
            s_note = "Ingen bilder p\xC3\xA5 minnekortet enn\xC3\xA5. Legg dem til under Bilder p\xC3\xA5 nettsiden til skjermen.";
            show_note();
            esp_lv_adapter_unlock();
        }
        return PIC_CARD_RETRY_MS;
    }
    const uint32_t stay_ms = (uint32_t)g_cfg->pic_secs * 1000u;
    const uint32_t since = lv_tick_get() - s_shown_tick;
    if (s_shown && !force && since < stay_ms) {
        sd_card_unlock();
        return stay_ms - since;
    }
    sd_card_unlock();

    /* The next one that can be shown, trying a few. The card is held only
     * while a file is read, not while it is decoded. */
    bool shown = false;
    for (int tries = 0; tries < 3 && !shown; tries++) {
        const int i = pic_bag_next(&s_bag, random32);
        if (i < 0) {
            break;
        }
        size_t len = 0;
        bool gone = false;
        sd_card_lock();
        uint8_t *jpg = sd_card_ready() == ESP_OK ? read_picture(s_names[i], &len) : NULL;
        if (jpg == NULL) {
            struct stat st;
            gone = stat(PIC_DIR, &st) != 0;
            if (gone) {
                sd_card_failed(); /* the card is gone */
                s_list_gen = 0;
            }
        }
        sd_card_unlock();
        if (gone) {
            break;
        }
        if (jpg != NULL) {
            shown = decode_into_screen(for_view, s_names[i], jpg, len);
            heap_caps_free(jpg);
        }
    }
    s_shown_tick = lv_tick_get();
    if (!shown) {
        /* With the last good one still on show, it stays, without a note. */
        if (!s_shown) {
            set_note(for_view, s_list_gen ? "Kunne ikke vise bildene p\xC3\xA5 minnekortet." : "Kan ikke lese minnekortet.");
        }
        return PIC_CARD_RETRY_MS;
    }
    ESP_LOGI(TAG, "Showing %s", s_on_show);
    return stay_ms;
}

/* ---- The /bilder page (web server) ---- */

/* Whether `n` is a name on the card the page may ask for: a picture's
 * name as the card lists it (UTF-8, long names too), and nothing that
 * leaves the folder. */
static bool name_ok(const char *n)
{
    const size_t len = strlen(n);
    if (len == 0 || len >= PIC_NAME_MAX || !pic_is_picture(n)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return strstr(n, "..") == NULL;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* GET /bilder */
static esp_err_t h_page(httpd_req_t *req)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "%s<title>Bilder - V&aelig;rtavla</title><h1>Bilder</h1>"
             "<p><a href=/>&larr; Navigasjon</a> &middot; <a href=/oppsett>Oppsett</a></p>"
             "<div id=pics><small>Henter bildene...</small></div><script src=/bilder.js></script>",
             wifi_provision_page_head());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

/* GET /bilder.js */
static esp_err_t h_js(httpd_req_t *req)
{
    extern const char pictures_js_start[] asm("_binary_pictures_page_js_start");
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, pictures_js_start);
}

/* Newest of the display's own first (by number), then the others by name. */
static int name_cmp(const void *a, const void *b)
{
    const char *x = a, *y = b;
    const long nx = pic_number(x), ny = pic_number(y);
    if (nx > 0 || ny > 0) {
        return nx == ny ? 0 : (nx > ny ? -1 : 1);
    }
    return strcmp(x, y);
}

/* GET /bilder.json: whether there is a card, its space, and the pictures. */
static esp_err_t h_list(httpd_req_t *req)
{
    /* The names and sizes in one pass over the folder, from FatFs itself:
     * a stat() per file would look its name up by reading the folder
     * again, n times over for n pictures. The name first, for name_cmp. */
    typedef struct {
        char n[PIC_NAME_MAX];
        uint32_t size;
    } entry_t;
    entry_t *ents = heap_caps_malloc((size_t)PIC_MAX * sizeof(entry_t), MALLOC_CAP_SPIRAM);
    FF_DIR *dir = heap_caps_malloc(sizeof(FF_DIR), MALLOC_CAP_SPIRAM);
    FILINFO *fi = heap_caps_malloc(sizeof(FILINFO), MALLOC_CAP_SPIRAM);
    cJSON *root = ents && dir && fi ? cJSON_CreateObject() : NULL;
    if (root == NULL) {
        heap_caps_free(ents);
        heap_caps_free(dir);
        heap_caps_free(fi);
        return httpd_resp_send_500(req);
    }
    sd_card_lock();
    const bool card = sd_card_ready() == ESP_OK;
    cJSON_AddBoolToObject(root, "kort", card);
    if (!card && sd_card_unreadable() != NULL) {
        cJSON_AddStringToObject(root, "kortfeil", sd_card_unreadable());
    }
    cJSON_AddNumberToObject(root, "maks", PIC_MAX);
    cJSON_AddNumberToObject(root, "maks_kb", PIC_UPLOAD_MAX / 1024);
    int n = 0;
    if (card) {
        uint64_t total = 0, free_b = 0;
        if (sd_card_space(&total, &free_b) == ESP_OK) {
            cJSON_AddNumberToObject(root, "mb_totalt", (double)(total / (1024 * 1024)));
            cJSON_AddNumberToObject(root, "mb_ledig", (double)(free_b / (1024 * 1024)));
        }
        char fpath[16];
        snprintf(fpath, sizeof(fpath), "%s/BILDER", sd_card_drive());
        FRESULT fr = f_opendir(dir, fpath);
        if (fr == FR_NO_PATH && mkdir(PIC_DIR, 0777) == 0) {
            fr = f_opendir(dir, fpath);
        }
        if (fr != FR_OK) {
            ESP_LOGW(TAG, "Couldn't open %s: %d", fpath, (int)fr);
            sd_card_failed();
            cJSON_ReplaceItemInObject(root, "kort", cJSON_CreateBool(false));
        } else {
            while (n < PIC_MAX && f_readdir(dir, fi) == FR_OK && fi->fname[0] != '\0') {
                if (!(fi->fattrib & AM_DIR) && strlen(fi->fname) < PIC_NAME_MAX && pic_is_picture(fi->fname)) {
                    snprintf(ents[n].n, PIC_NAME_MAX, "%s", fi->fname);
                    ents[n++].size = fi->fsize > UINT32_MAX ? UINT32_MAX : (uint32_t)fi->fsize;
                }
            }
            f_closedir(dir);
            qsort(ents, (size_t)n, sizeof(entry_t), name_cmp);
        }
    }
    sd_card_unlock();
    heap_caps_free(dir);
    heap_caps_free(fi);
    cJSON *list = cJSON_AddArrayToObject(root, "bilder");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", ents[i].n);
        cJSON_AddNumberToObject(o, "kb", (ents[i].size + 1023) / 1024);
        cJSON_AddBoolToObject(o, "for_stor", ents[i].size > PIC_FILE_MAX);
        cJSON_AddItemToArray(list, o);
    }
    heap_caps_free(ents);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = send_json(req, NULL, json);
    cJSON_free(json);
    return err;
}

/* GET /bilder/fil?n=B0000001.JPG: the picture, for the page's list. */
static esp_err_t h_file(httpd_req_t *req)
{
    /* A long name's every byte may come %-escaped. */
    char q[3 * PIC_NAME_MAX + 8], n[PIC_NAME_MAX + 4];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || !form_field(q, "n", n, sizeof(n)) ||
        !name_ok(n)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Ukjent bilde.");
    }
    /* Read whole, then sent with the card let go: a slow phone loading the
     * list's pictures one by one would otherwise hold the card - and the
     * picture screen with it - for as long as each takes to arrive. */
    size_t len = 0;
    sd_card_lock();
    uint8_t *jpg = sd_card_ready() == ESP_OK ? read_picture(n, &len) : NULL;
    sd_card_unlock();
    if (jpg == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "Fant ikke bildet.");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const esp_err_t err = httpd_resp_send(req, (const char *)jpg, (ssize_t)len);
    heap_caps_free(jpg);
    return err;
}

/* The next of the display's own names (SD lock held): one past the
 * highest on the card. */
static long next_number(void)
{
    long most = 0;
    DIR *d = opendir(PIC_DIR);
    if (d == NULL) {
        return mkdir(PIC_DIR, 0777) == 0 ? 1 : -1;
    }
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        const long k = pic_number(e->d_name);
        most = k > most ? k : most;
        count += pic_is_picture(e->d_name);
    }
    closedir(d);
    if (count >= PIC_MAX || most >= 9999999) {
        return 0; /* full */
    }
    return most + 1;
}

/* POST /bilder/last-opp: one JPEG as the body, from the page. */
static esp_err_t h_upload(httpd_req_t *req)
{
    if (req->content_len < 4 || req->content_len > PIC_UPLOAD_MAX) {
        return send_json(req, "413 Payload Too Large", "{\"feil\":\"for stor\"}");
    }
    const size_t len = req->content_len;
    uint8_t *body = psram_room(len)
                        ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM)
                        : NULL;
    if (body == NULL) {
        return send_json(req, "507 Insufficient Storage", "{\"feil\":\"minne\"}");
    }
    /* All of it first, so the card isn't held while it comes. */
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        const int r = httpd_req_recv(req, (char *)body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (r <= 0) {
            heap_caps_free(body);
            return send_json(req, "400 Bad Request", "{\"feil\":\"mottak\"}");
        }
        got += (size_t)r;
    }
    if (body[0] != 0xFF || body[1] != 0xD8 || body[2] != 0xFF) {
        heap_caps_free(body);
        return send_json(req, "415 Unsupported Media Type", "{\"feil\":\"ikke jpeg\"}");
    }

    const char *status = NULL, *json = NULL;
    char name[PIC_NAME_MAX], ok_json[PIC_NAME_MAX + 16];
    sd_card_lock();
    uint64_t total = 0, free_b = 0;
    long num = -1;
    if (sd_card_ready() != ESP_OK) {
        status = "503 Service Unavailable";
        json = "{\"feil\":\"kort\"}";
    } else if (sd_card_space(&total, &free_b) == ESP_OK && free_b < len + 64 * 1024) {
        status = "507 Insufficient Storage";
        json = "{\"feil\":\"fullt\"}";
    } else if ((num = next_number()) <= 0) {
        status = num == 0 ? "507 Insufficient Storage" : "500 Internal Server Error";
        json = num == 0 ? "{\"feil\":\"mange\"}" : "{\"feil\":\"kort\"}";
        if (num < 0) {
            sd_card_failed();
        }
    } else {
        /* Under another name until it is all there, so the screen never
         * picks up half a picture. */
        char tmp[PIC_NAME_MAX], path[sizeof(PIC_DIR) + PIC_NAME_MAX + 1], tmp_path[sizeof(path)];
        pic_name(tmp, num, "TMP");
        pic_name(name, num, "JPG");
        snprintf(tmp_path, sizeof(tmp_path), PIC_DIR "/%s", tmp);
        snprintf(path, sizeof(path), PIC_DIR "/%s", name);
        FILE *f = fopen(tmp_path, "wb");
        bool ok = f != NULL && fwrite(body, 1, len, f) == len;
        if (f != NULL) {
            ok &= fflush(f) == 0 && fsync(fileno(f)) == 0;
            ok &= fclose(f) == 0;
        }
        ok = ok && rename(tmp_path, path) == 0;
        if (!ok) {
            unlink(tmp_path);
            ESP_LOGE(TAG, "Couldn't save %s: errno %d", name, errno);
            sd_card_failed();
            status = "500 Internal Server Error";
            json = "{\"feil\":\"lagring\"}";
        } else {
            s_gen++;
            snprintf(ok_json, sizeof(ok_json), "{\"n\":\"%s\"}", name);
            json = ok_json;
            ESP_LOGI(TAG, "Saved %s (%u bytes)", name, (unsigned)len);
        }
    }
    sd_card_unlock();
    heap_caps_free(body);
    if (status == NULL) {
        app_wake_poll();
    }
    return send_json(req, status, json);
}

/* POST /bilder/slett: n=B0000001.JPG */
static esp_err_t h_delete(httpd_req_t *req)
{
    char body[3 * PIC_NAME_MAX + 8], n[PIC_NAME_MAX + 4];
    int got = req->content_len < sizeof(body) ? httpd_req_recv(req, body, req->content_len) : -1;
    if (got <= 0) {
        return send_json(req, "400 Bad Request", "{\"feil\":\"skjema\"}");
    }
    body[got] = '\0';
    if (!form_field(body, "n", n, sizeof(n)) || !name_ok(n)) {
        return send_json(req, "400 Bad Request", "{\"feil\":\"navn\"}");
    }
    char path[PIC_PATH_MAX];
    snprintf(path, sizeof(path), PIC_DIR "/%s", n);
    sd_card_lock();
    const bool card = sd_card_ready() == ESP_OK;
    const bool ok = card && unlink(path) == 0;
    if (ok) {
        s_gen++;
    }
    sd_card_unlock();
    if (!ok) {
        return send_json(req, card ? "404 Not Found" : "503 Service Unavailable",
                         card ? "{\"feil\":\"finnes ikke\"}" : "{\"feil\":\"kort\"}");
    }
    ESP_LOGI(TAG, "Deleted %s", n);
    app_wake_poll();
    return send_json(req, NULL, "{}");
}

/* GET /bilder/hent?url=...: a picture from another site, passed on to the
 * page to shrink. In a task of its own (TLS needs more stack than the web
 * server's, and the web server goes on meanwhile), one at a time. Only
 * what starts like a picture is passed on, so it isn't a way to read
 * anything else through the display. */
typedef struct {
    httpd_req_t *req;
    char url[1024];
    uint8_t head[32]; /* the first bytes, to tell it is a picture */
    size_t head_len;
    bool started;     /* the answer's headers are sent */
    bool refused;     /* not a picture */
    bool send_failed;
} fetch_job_t;

static volatile bool s_fetching;


static bool fetch_chunk(const char *data, size_t len, void *ctx)
{
    fetch_job_t *j = ctx;
    if (!j->started) {
        /* Hold the first bytes until there are enough to tell. */
        const size_t take = len < sizeof(j->head) - j->head_len ? len : sizeof(j->head) - j->head_len;
        memcpy(j->head + j->head_len, data, take);
        j->head_len += take;
        data += take;
        len -= take;
        if (j->head_len < sizeof(j->head) && len == 0) {
            return true;
        }
        if (!pic_looks_like_picture(j->head, j->head_len)) {
            j->refused = true;
            return false;
        }
        httpd_resp_set_type(j->req, "application/octet-stream");
        httpd_resp_set_hdr(j->req, "Cache-Control", "no-store");
        j->started = true;
        if (httpd_resp_send_chunk(j->req, (const char *)j->head, (ssize_t)j->head_len) != ESP_OK) {
            j->send_failed = true;
            return false;
        }
    }
    if (len > 0 && httpd_resp_send_chunk(j->req, data, (ssize_t)len) != ESP_OK) {
        j->send_failed = true;
        return false;
    }
    return true;
}

static void fetch_task(void *arg)
{
    fetch_job_t *j = arg;
    char ua[48];
    snprintf(ua, sizeof(ua), "MultiDisplay/%s", esp_app_get_description()->version);
    const esp_err_t err = http_get_stream(j->url, ua, PIC_FETCH_TIMEOUT_MS, PIC_FETCH_MAX, true, fetch_chunk, j, TAG);
    if (!j->started && err == ESP_OK && j->head_len > 0) {
        /* A picture smaller than the bytes held back. */
        if (pic_looks_like_picture(j->head, j->head_len)) {
            httpd_resp_set_type(j->req, "application/octet-stream");
            httpd_resp_send(j->req, (const char *)j->head, (ssize_t)j->head_len);
        } else {
            send_json(j->req, "415 Unsupported Media Type", "{\"feil\":\"ikke bilde\"}");
        }
    } else if (!j->started) {
        send_json(j->req, j->refused ? "415 Unsupported Media Type" : "502 Bad Gateway",
                  j->refused ? "{\"feil\":\"ikke bilde\"}" : "{\"feil\":\"henting\"}");
    } else if (err == ESP_OK && !j->send_failed) {
        httpd_resp_send_chunk(j->req, NULL, 0);
    } else {
        /* Partly sent: cut the connection, so the page sees it failed
         * rather than taking half a picture. */
        httpd_sess_trigger_close(j->req->handle, httpd_req_to_sockfd(j->req));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Fetching a picture for the page failed");
    }
    httpd_req_async_handler_complete(j->req);
    heap_caps_free(j);
    s_fetching = false;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t h_fetch(httpd_req_t *req)
{
    fetch_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM);
    if (j == NULL) {
        return send_json(req, "507 Insufficient Storage", "{\"feil\":\"minne\"}");
    }
    const size_t qlen = httpd_req_get_url_query_len(req);
    char *q = qlen > 0 && qlen < 3 * sizeof(j->url) ? heap_caps_malloc(qlen + 1, MALLOC_CAP_SPIRAM) : NULL;
    esp_err_t ret;
    if (q == NULL || httpd_req_get_url_query_str(req, q, qlen + 1) != ESP_OK ||
        !form_field(q, "url", j->url, sizeof(j->url)) || strlen(j->url) >= sizeof(j->url) - 1 || /* never cut */
        (strncmp(j->url, "http://", 7) != 0 && strncmp(j->url, "https://", 8) != 0)) {
        ret = send_json(req, "400 Bad Request", "{\"feil\":\"adresse\"}");
    } else if (s_fetching) {
        ret = send_json(req, "429 Too Many Requests", "{\"feil\":\"opptatt\"}");
    } else if (httpd_req_async_handler_begin(req, &j->req) != ESP_OK) {
        ret = send_json(req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
    } else {
        heap_caps_free(q);
        s_fetching = true;
        /* The stack in PSRAM: this task only fetches and sends, nothing
         * that maps flash. */
        if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "picfetch", PIC_FETCH_STACK, j, 4, NULL, tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            return ESP_OK; /* the task answers, completes the request and frees the job */
        }
        s_fetching = false;
        ret = send_json(j->req, "500 Internal Server Error", "{\"feil\":\"minne\"}");
        httpd_req_async_handler_complete(j->req);
        heap_caps_free(j);
        return ret;
    }
    heap_caps_free(q);
    heap_caps_free(j);
    return ret;
}

void pictures_add_handlers(void)
{
    wifi_provision_add_page_handler("/bilder", h_page);
    wifi_provision_add_get_handler("/bilder.js", h_js);
    wifi_provision_add_get_handler("/bilder.json", h_list);
    wifi_provision_add_get_handler("/bilder/fil", h_file);
    wifi_provision_add_get_handler("/bilder/hent", h_fetch);
    wifi_provision_add_post_handler("/bilder/last-opp", h_upload);
    wifi_provision_add_post_handler("/bilder/slett", h_delete);
}
