/* The colour theme (see theme.h): light or dark, and the Christmas
 * decorations in either.
 *
 * The decorations are small tree ornaments hanging from the top edge of
 * every screen - red baubles, a gold one and a star, between two sprigs of
 * pine - and LVGL's own widgets with red buttons and gold as the
 * second colour. The ornaments are drawn by one object that is each
 * screen's first child (theme_decorate, from screen_root_create), so the
 * screen's own text and pictures are drawn over them, and where a text
 * reaches into them it gets the background's colour under it (draw_plates):
 * they show in the gaps and never hide anything. Nothing moves. Tried and taken out: a pine
 * green background with cream text, and snow - falling (moved ten times a
 * second) it made the panel tear and jump, as every redraw competes with
 * the panel's refill from PSRAM.
 *
 * "Fra 1. desember til 6. januar" (APP_XMAS_SEASON) has them through the
 * season. The screens are built once, so the season is read at boot: the
 * nightly restart at 02:00 brings them in on 1 December and takes them
 * away on 7 January. After a power cut the clock isn't known at boot, so
 * the last season is remembered in NVS, and once the clock is set main.c
 * restarts if the date says otherwise (theme_season_changed). */

#include <time.h>

#include "esp_log.h"

#include "app.h"
#include "draw.h"
#include "misc/lv_area_private.h"
#include "theme.h"

static const char *TAG = "theme";

static bool s_dark;
static bool s_xmas;

/* The season "Jul fra 1. desember til 6. januar" covers. */
static bool in_season(const struct tm *lt)
{
    return lt->tm_mon == 11 || (lt->tm_mon == 0 && lt->tm_mday <= 6);
}

/* Decorated by the settings and `season` (whether the date is in it). */
static bool wants_xmas(bool season)
{
    return g_cfg->xmas == APP_XMAS_ON || (g_cfg->xmas == APP_XMAS_SEASON && season);
}

void theme_init(void)
{
    bool season;
    const time_t now = time(NULL);
    if (now > PLAUSIBLE_EPOCH_S) { /* kept over a restart, not a power cut */
        struct tm lt;
        localtime_r(&now, &lt);
        season = in_season(&lt);
    } else {
        season = app_config_load_xmas_now() != 0;
    }
    s_xmas = wants_xmas(season);
    s_dark = g_cfg->theme == APP_THEME_DARK;
    if (g_cfg->xmas == APP_XMAS_SEASON && (app_config_load_xmas_now() != 0) != season) {
        app_config_save_xmas_now(season);
    }
    ESP_LOGI(TAG, "%s theme%s%s", s_dark ? "Dark" : "Light", s_xmas ? ", decorated for Christmas" : "",
             g_cfg->xmas == APP_XMAS_SEASON ? (season ? " (in the season)" : " (outside the season)") : "");
}

bool theme_dark(void)
{
    return s_dark;
}

bool theme_christmas(void)
{
    return s_xmas;
}

#define XMAS_RED  0xC0392B
#define XMAS_DEEP 0x8E1B12 /* an ornament's shaded edge */
#define XMAS_GOLD 0xD4A72C

uint32_t theme_hex(uint32_t dark, uint32_t light)
{
    return s_dark ? dark : light;
}

lv_color_t theme_color(uint32_t dark, uint32_t light)
{
    return lv_color_hex(theme_hex(dark, light));
}

lv_color_t theme_color_if(bool dark, uint32_t dark_hex, uint32_t light_hex)
{
    return lv_color_hex(dark ? dark_hex : light_hex);
}

#define XMAS_GOLD_DEEP 0x9A7715
#define XMAS_PINE      0x2E7D4F
#define XMAS_PINE_LITE 0x4FA36E
#define XMAS_TWIG      0x6B4A2B

/* What hangs from the top edge, each on a gold thread: red baubles, a gold
 * one and a star, with a sprig of pine at either end. */
typedef enum { HANG_RED, HANG_GOLD, HANG_STAR } hang_t;

typedef struct {
    int16_t x;    /* the thread, from the left */
    uint8_t drop; /* the thread's length */
    uint8_t r;    /* the bauble's radius */
    uint8_t kind; /* hang_t */
} ornament_t;

static const ornament_t ORNAMENTS[] = {
    { 286, 10, 6, HANG_RED }, { 322, 22, 7, HANG_RED },  { 352, 6, 5, HANG_RED },  { 388, 16, 6, HANG_RED },
    { 420, 20, 12, HANG_STAR }, { 452, 8, 5, HANG_RED },  { 484, 20, 6, HANG_GOLD }, { 516, 12, 7, HANG_RED },
};
#define ORNAMENT_COUNT (sizeof(ORNAMENTS) / sizeof(ORNAMENTS[0]))
#define DECOR_H        48 /* the deepest hangs within this */
#define SPRIG_LEFT_X   262
#define SPRIG_RIGHT_X  540

/* A five-pointed star, 12 px from its middle (cx, cy) to its points. */
static void draw_star(lv_layer_t *layer, int cx, int cy, lv_color_t colour)
{
    static const int8_t OUT[5][2] = { { 0, -12 }, { 11, -4 }, { 7, 10 }, { -7, 10 }, { -11, -4 } };
    static const int8_t IN[5][2] = { { 3, -4 }, { 5, 2 }, { 0, 5 }, { -5, 2 }, { -3, -4 } }; /* IN[k] after OUT[k] */
    for (int k = 0; k < 5; k++) {
        const int8_t *a = IN[(k + 4) % 5], *b = IN[k];
        const int tip[3][2] = { { cx + OUT[k][0], cy + OUT[k][1] }, { cx + a[0], cy + a[1] }, { cx + b[0], cy + b[1] } };
        draw_triangle(layer, tip, colour);
        const int mid[3][2] = { { cx, cy }, { cx + a[0], cy + a[1] }, { cx + b[0], cy + b[1] } };
        draw_triangle(layer, mid, colour);
    }
}

/* A sprig of pine from the top edge at x, leaning to the side `dir` (-1
 * left, 1 right): a twig with needles off both sides. */
static void draw_sprig(lv_layer_t *layer, int x, int top, int dir)
{
    const int dx = 15 * dir, dy = 24;
    for (int i = 0; i <= 6; i++) {
        const int px = x + dx * i / 6, py = top + dy * i / 6;
        const lv_color_t green = lv_color_hex(i % 2 ? XMAS_PINE_LITE : XMAS_PINE);
        draw_line(layer, px, py, px + 9 * dir, py + 3, 2, green); /* outwards */
        draw_line(layer, px, py, px - 3 * dir, py + 9, 2, green); /* downwards */
    }
    draw_line(layer, x, top, x + dx, top + dy, 2, lv_color_hex(XMAS_TWIG));
}

/* The screen's texts that reach into `within` (the decorations) get the
 * background's colour under them, nearly covering: the decorations are
 * drawn first, so this lies over them and under the text, which stays
 * readable. Only the text's own area - a label redraws that when it
 * changes, and so this with it. */
#define PLATE_OPA LV_OPA_80

static void draw_plates(lv_layer_t *layer, lv_obj_t *parent, const lv_obj_t *decor, const lv_area_t *within,
                        lv_draw_rect_dsc_t *plate)
{
    const uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(parent, (int32_t)i);
        if (o == decor || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_check_type(o, &lv_label_class)) {
            const char *txt = lv_label_get_text(o);
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            if (txt == NULL || txt[0] == '\0' || a.y1 > within->y2 || a.y2 < within->y1) {
                continue;
            }
            /* The text where it stands in a label wider than it. */
            lv_point_t size;
            lv_text_get_size(&size, txt, lv_obj_get_style_text_font(o, LV_PART_MAIN),
                             lv_obj_get_style_text_letter_space(o, LV_PART_MAIN),
                             lv_obj_get_style_text_line_space(o, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const int32_t spare = lv_area_get_width(&a) - size.x;
            if (spare > 0) {
                const lv_text_align_t align = lv_obj_get_style_text_align(o, LV_PART_MAIN);
                if (align == LV_TEXT_ALIGN_RIGHT) {
                    a.x1 += spare;
                } else if (align == LV_TEXT_ALIGN_CENTER) {
                    a.x1 += spare / 2;
                    a.x2 -= spare - spare / 2;
                } else {
                    a.x2 -= spare;
                }
            }
            lv_area_t over;
            if (lv_area_intersect(&over, &a, within) && draw_area_visible(layer, &over)) {
                lv_draw_rect(layer, plate, &over);
            }
        } else {
            draw_plates(layer, o, decor, within, plate);
        }
    }
}

static void decor_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t at;
    lv_obj_get_coords(lv_event_get_current_target(e), &at);
    if (!draw_area_visible(layer, &at)) {
        return;
    }
    draw_sprig(layer, at.x1 + SPRIG_LEFT_X, at.y1, -1);
    draw_sprig(layer, at.x1 + SPRIG_RIGHT_X, at.y1, 1);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(XMAS_GOLD);
    for (size_t i = 0; i < ORNAMENT_COUNT; i++) {
        const ornament_t *o = &ORNAMENTS[i];
        const int x = at.x1 + o->x, y = at.y1 + o->drop;
        const lv_area_t thread = { x, at.y1, x, y - 1 };
        d.bg_opa = LV_OPA_70;
        lv_draw_rect(layer, &d, &thread);
        if (o->kind == HANG_STAR) {
            draw_star(layer, x, y + o->r, lv_color_hex(XMAS_GOLD));
            continue;
        }
        const bool gold = o->kind == HANG_GOLD;
        const lv_area_t cap = { x - 2, y, x + 2, y + 2 };
        d.bg_opa = LV_OPA_COVER;
        d.bg_color = lv_color_hex(gold ? XMAS_GOLD_DEEP : XMAS_GOLD);
        lv_draw_rect(layer, &d, &cap);
        d.bg_color = lv_color_hex(XMAS_GOLD);
        const int cy = y + 2 + o->r;
        draw_dot(layer, x, cy, o->r, lv_color_hex(gold ? XMAS_GOLD_DEEP : XMAS_DEEP)); /* the shaded edge */
        draw_dot(layer, x, cy, o->r - 1, lv_color_hex(gold ? XMAS_GOLD : XMAS_RED));
        draw_dot(layer, x - o->r / 3, cy - o->r / 3, 1, lv_color_hex(gold ? 0xFFF3C4 : 0xF6C9C4)); /* the glint */
    }

    /* Under the screen's texts: the colour of whatever is behind this. */
    lv_obj_t *self = lv_event_get_current_target(e);
    lv_obj_t *root = lv_obj_get_parent(self);
    lv_draw_rect_dsc_t plate;
    lv_draw_rect_dsc_init(&plate);
    plate.bg_opa = PLATE_OPA;
    for (lv_obj_t *o = root; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) >= LV_OPA_50) {
            plate.bg_color = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
            break;
        }
    }
    draw_plates(layer, root, self, &at, &plate);
}

void theme_decorate(lv_obj_t *root)
{
    if (!s_xmas) {
        return;
    }
    lv_obj_t *o = lv_obj_create(root);
    if (o == NULL) {
        return;
    }
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, LV_PCT(100), DECOR_H);
    lv_obj_add_event_cb(o, decor_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

void theme_apply(lv_obj_t *screen)
{
    lv_display_t *disp = lv_obj_get_display(screen);
    const lv_color_t primary = s_xmas ? lv_color_hex(XMAS_RED) : lv_palette_main(LV_PALETTE_BLUE);
    const lv_color_t secondary = s_xmas ? lv_color_hex(XMAS_GOLD) : lv_palette_main(LV_PALETTE_RED);
    lv_display_set_theme(disp, lv_theme_default_init(disp, primary, secondary, s_dark, g_font_body));
    lv_theme_apply(screen);
}

bool theme_season_changed(void)
{
    if (g_cfg->xmas != APP_XMAS_SEASON) {
        return false;
    }
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    const bool season = in_season(&lt);
    if (wants_xmas(season) == s_xmas) {
        return false;
    }
    app_config_save_xmas_now(season);
    return true;
}
