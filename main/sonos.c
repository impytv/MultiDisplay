/* The Sonos screen: what plays in a room of Sonos speakers on the local
 * network - the cover, the title, the artist and the album, how far it has
 * got - with buttons for the previous track, play/pause, the next track
 * and the volume. A row of buttons at the top picks the room; while it
 * plays, a tap on an idle room adds it to the group playing (as grouping in
 * the Sonos app), and a tap on a room in the group takes it out. "Velg
 * musikk" swaps the player for a grid of the household's Sonos favourites
 * and what the display's Spotify account played lately (the playlist or
 * album it was played from), and a tap starts one in the room.
 *
 * The speakers are asked directly (components/sonos_client.h): no login,
 * nor Sonos's cloud. "Recently played" needs the Spotify screen's login,
 * though not the screen itself, and Spotify added in the Sonos app.
 *
 * While on show the room is asked every few seconds, and at once after a
 * button; the progress bar moves on by itself between. As on the Spotify
 * screen, the buttons are only drawn: the tap layer hands touches here
 * (sonos_tap) first, and the weather task sends the command. The covers'
 * memory is only held while the screen is on show (sonos_release). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "diag.h"
#include "draw.h"
#include "sonos.h"
#include "sonos_client.h"
#include "spotify_client.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "sonos_screen";

#define SO_X          12
#define SO_ROW_Y      58  /* the room buttons */
#define SO_ROW_H      44
#define SO_CHIP_PAD   18
#define SO_CHIP_GAP   10
#define SO_TOP        118 /* below them */
#define SO_COVER      240
#define SO_COL_X      (SO_X + SO_COVER + 28) /* the column right of the cover */
#define SO_COL_W      (BOARD_LCD_H_RES - SO_COL_X - SO_X)
#define SO_PROG_Y     292 /* the progress bar */
#define SO_CTRL_Y     360 /* the middle of the playback buttons */
#define SO_PLAY_R     38
#define SO_SKIP_R     30
#define SO_SKIP_DX    115
#define SO_VOL_Y      438 /* the middle of the volume row */
#define SO_VOL_R      22
#define SO_VOL_STEP   3
#define SO_TILE       104 /* the picker's covers (a multiple of 8, for the decoder) */
#define SO_TILE_COLS  6
#define SO_TILES      (2 * SO_TILE_COLS)
#define SO_PLAYING_MS 3000
#define SO_IDLE_MS    6000
#define SO_RETRY_MS   15000
#define SO_ROOMS_MS   30000           /* the rooms and groups asked again */
#define SO_LISTS_MS   (2 * 60 * 1000) /* favourites and recently played, while picking */
#define SO_SETTLE_MS  500             /* after a command, before asking what plays */
#define SO_NOTE_MS    10000
#define SO_TICK_MS    500
#define SO_NAMES_MAX  24              /* playlists' names remembered */
#define SO_NAMES_PER_POLL 4

static lv_obj_t *s_root, *s_canvas, *s_title, *s_track, *s_artist, *s_album, *s_msg;
static big_clock_t s_clock;

/* Under the adapter lock; written by the weather task unless said. */
static EXT_RAM_BSS_ATTR sonos_room_t s_rooms[SONOS_ROOMS_MAX];
static int s_room_count;
static int s_room = -1;          /* the room on show, with its group; set by a tap too */
static bool s_busy[SONOS_ROOMS_MAX]; /* the room's group plays (another than s_room's) */
static char s_room_name[48];     /* its name, to find it again; "" = not chosen yet */
static EXT_RAM_BSS_ATTR sonos_now_t s_now;
static bool s_have;              /* s_now holds an answer for s_room */
static uint32_t s_now_tick;      /* lv_tick_get() when it came */
static bool s_woke_at_end;
static bool s_picker;            /* the picker on show, not the player; set by taps */
static bool s_picker_shut;       /* closed by a tap: not opened again for an empty room */
static char s_note[96];          /* a message for a while, "" = none */
static uint32_t s_note_tick;
static char s_list_note[160];    /* the picker's own, while it lasts */

/* The picker's tiles. A favourite is played by its index in s_favs (the
 * weather task's), a Spotify item by its URI. The covers' pixels are the
 * weather task's to allocate and fill, with `ok` false meanwhile. */
typedef struct {
    bool fav;
    int fav_idx;
    char uri[64];
    char title[64];
    char art[SONOS_URL_MAX];
    uint16_t *px;            /* SO_TILE square, RGB565, PSRAM; NULL = none yet */
    char px_art[SONOS_URL_MAX]; /* the picture in px */
    bool ok;
    lv_image_dsc_t img;
} tile_t;
static EXT_RAM_BSS_ATTR tile_t s_tiles[SO_TILES];
static int s_tile_count;

/* The player's cover, like the tiles'. */
static uint16_t *s_cover_px;
static lv_image_dsc_t s_cover_img;
static char s_cover_url[SONOS_URL_MAX];
static bool s_cover_ok;

/* Buttons pressed, for the weather task to send in turn. A tap fills
 * s_cmd, and send() queues it with the group on show as its target, so a
 * room chosen next doesn't get it. */
typedef enum { CMD_NONE, CMD_SONOS, CMD_FAV, CMD_SPOTIFY, CMD_JOIN, CMD_LEAVE } cmd_kind_t;
typedef struct {
    cmd_kind_t kind;
    sonos_room_t target; /* the group's leader, for playback */
    sonos_room_t zone;   /* the room joining or leaving */
    char coord[32];      /* the group it joins */
    sonos_cmd_t cmd;
    int value;
    int fav;
    char uri[64];
} cmd_t;
#define SO_CMDS 6
static cmd_t s_cmd;
static EXT_RAM_BSS_ATTR cmd_t s_cmds[SO_CMDS];
static int s_cmd_count;

/* The weather task's own. */
static EXT_RAM_BSS_ATTR sonos_room_t s_room_scratch[SONOS_ROOMS_MAX];
static EXT_RAM_BSS_ATTR sonos_now_t s_now_scratch;
static sonos_fav_t *s_favs; /* SONOS_FAVS_MAX, PSRAM */
static int s_fav_count;
static EXT_RAM_BSS_ATTR spotify_recent_t s_recent[SPOTIFY_RECENT_MAX];
static fetch_stamp_t s_rooms_at, s_lists_at;
static bool s_rediscover;
static backoff_t s_fail;
static struct {
    char uri[64];
    char name[64];
    char image[160];
    bool missing; /* Spotify won't say (its own playlists) */
} s_names[SO_NAMES_MAX] EXT_RAM_BSS_ATTR;
static int s_names_next;

static bool dark(void)
{
    return g_cfg->theme == APP_THEME_DARK || g_cfg->so_dark;
}

static lv_color_t c_txt(void)
{
    return lv_color_hex(dark() ? 0xDDE6EE : 0x1B2631);
}

static lv_color_t c_dim(void)
{
    return lv_color_hex(dark() ? 0x8AA0B4 : 0x5D6D7E);
}

static lv_color_t c_card(void)
{
    return lv_color_hex(dark() ? 0x1E2A36 : 0xE3E8EE);
}

static lv_color_t c_bg(void)
{
    return lv_color_hex(dark() ? 0x15171A : 0xFFFFFF);
}

static lv_color_t c_accent(void)
{
    return lv_color_hex(0xE8A33D); /* warm, unlike Spotify's green */
}

static void mmss(char *out, size_t n, int s)
{
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static bool playing(void)
{
    return s_now.state == SONOS_PLAYING || s_now.state == SONOS_TRANSITIONING;
}

/* Where the track has got to now, in seconds, from the last answer. */
static int position_now(void)
{
    int p = s_now.position_s < 0 ? 0 : s_now.position_s;
    if (playing()) {
        p += (int)((lv_tick_get() - s_now_tick) / 1000);
    }
    return s_now.duration_s > 0 && p > s_now.duration_s ? s_now.duration_s : p;
}

/* Whether answer b shows anything a didn't. Not the position while it
 * plays, which the progress bar moves on by itself (sonos_tick_cb). */
static bool now_changed(const sonos_now_t *a, const sonos_now_t *b)
{
    const bool moving = b->state == SONOS_PLAYING || b->state == SONOS_TRANSITIONING;
    return a->state != b->state || a->empty != b->empty || a->stream != b->stream ||
           a->duration_s != b->duration_s || (!moving && a->position_s != b->position_s) ||
           a->can_play != b->can_play || a->can_pause != b->can_pause || a->can_next != b->can_next ||
           a->can_previous != b->can_previous || a->volume != b->volume || strcmp(a->source, b->source) != 0 ||
           strcmp(a->title, b->title) != 0 || strcmp(a->artist, b->artist) != 0 || strcmp(a->album, b->album) != 0;
}

static void note(const char *msg)
{
    snprintf(s_note, sizeof(s_note), "%s", msg);
    s_note_tick = lv_tick_get();
}

/* The room leading room i's group (adapter lock held). */
static int lead_of(int i)
{
    for (int k = 0; k < s_room_count; k++) {
        if (strcmp(s_rooms[k].uuid, s_rooms[i].coord) == 0) {
            return k;
        }
    }
    return i;
}

/* Whether room i plays along with the room on show. */
static bool in_group(int i)
{
    return s_room >= 0 && strcmp(s_rooms[i].coord, s_rooms[s_room].coord) == 0;
}

static int group_size(void)
{
    int n = 0;
    for (int i = 0; i < s_room_count; i++) {
        n += in_group(i);
    }
    return n;
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */

static const char *toggle_text(void)
{
    return s_picker ? "Spiller n\xC3\xA5" : "Velg musikk";
}

/* The picker's button, at the right of the row. */
static void toggle_box(lv_area_t *a)
{
    a->x2 = BOARD_LCD_H_RES - SO_X - 1;
    a->x1 = a->x2 - draw_text_w("Velg musikk") - 2 * SO_CHIP_PAD;
    a->y1 = SO_ROW_Y;
    a->y2 = SO_ROW_Y + SO_ROW_H - 1;
}

/* Room button i's box; false if it doesn't fit before the picker's. */
static bool chip_box(int i, lv_area_t *a)
{
    lv_area_t t;
    toggle_box(&t);
    int x = SO_X;
    for (int k = 0; k <= i && k < s_room_count; k++) {
        const int w = draw_text_w(s_rooms[k].name) + 2 * SO_CHIP_PAD;
        if (k == i) {
            a->x1 = x;
            a->x2 = x + w - 1;
            a->y1 = SO_ROW_Y;
            a->y2 = SO_ROW_Y + SO_ROW_H - 1;
            return a->x2 < t.x1 - SO_CHIP_GAP;
        }
        x += w + SO_CHIP_GAP;
    }
    return false;
}

static void tile_box(int i, lv_area_t *a)
{
    const int gap = (BOARD_LCD_H_RES - 2 * SO_X - SO_TILE_COLS * SO_TILE) / (SO_TILE_COLS - 1);
    const int name_h = 2 * lv_font_get_line_height(g_font_body); /* two lines */
    a->x1 = SO_X + (i % SO_TILE_COLS) * (SO_TILE + gap);
    a->y1 = SO_TOP + (i / SO_TILE_COLS) * (SO_TILE + 6 + name_h + 6);
    a->x2 = a->x1 + SO_TILE - 1;
    a->y2 = a->y1 + SO_TILE + 6 + name_h - 1; /* with its name */
}

/* --------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

static void draw_image(lv_layer_t *layer, const lv_image_dsc_t *img, int x, int y, int side)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = img;
    lv_area_t a = { x, y, x + side - 1, y + side - 1 };
    lv_draw_image(layer, &d, &a);
}

static void draw_chip(lv_layer_t *layer, const lv_area_t *a, const char *text, bool selected, lv_color_t fill)
{
    draw_rect(layer, a->x1, a->y1, a->x2, a->y2, SO_ROW_H / 2, selected ? c_txt() : fill);
    const int lh = lv_font_get_line_height(g_font_body);
    draw_text(layer, text, a->x1, a->y1 + (SO_ROW_H - lh) / 2, a->x2 - a->x1 + 1, LV_TEXT_ALIGN_CENTER,
              selected ? c_bg() : c_txt());
}

static void draw_row(lv_layer_t *layer)
{
    for (int i = 0; i < s_room_count; i++) {
        lv_area_t a;
        if (chip_box(i, &a)) {
            draw_chip(layer, &a, s_rooms[i].name, in_group(i), c_card());
            if (s_busy[i] && !in_group(i)) { /* plays something of its own */
                draw_dot(layer, a.x2 - 9, a.y1 + 9, 5, c_accent());
            }
        }
    }
    if (s_room_count > 0) {
        lv_area_t a;
        toggle_box(&a);
        draw_rect(layer, a.x1, a.y1, a.x2, a.y2, SO_ROW_H / 2, c_accent());
        const int lh = lv_font_get_line_height(g_font_body);
        draw_text(layer, toggle_text(), a.x1, a.y1 + (SO_ROW_H - lh) / 2, a.x2 - a.x1 + 1, LV_TEXT_ALIGN_CENTER,
                  lv_color_hex(0x1B2631));
    }
}

static void draw_play_button(lv_layer_t *layer, int cx, int cy, bool show_pause, bool locked)
{
    draw_dot(layer, cx, cy, SO_PLAY_R, locked ? c_card() : c_accent());
    const lv_color_t ink = locked ? c_dim() : lv_color_hex(0x1B2631);
    if (show_pause) {
        draw_rect(layer, cx - 13, cy - 15, cx - 5, cy + 15, 2, ink);
        draw_rect(layer, cx + 5, cy - 15, cx + 13, cy + 15, 2, ink);
    } else {
        const int pts[3][2] = { { cx - 10, cy - 17 }, { cx - 10, cy + 17 }, { cx + 18, cy } };
        draw_triangle(layer, pts, ink);
    }
}

static void draw_stop_button(lv_layer_t *layer, int cx, int cy)
{
    draw_dot(layer, cx, cy, SO_PLAY_R, c_accent());
    draw_rect(layer, cx - 14, cy - 14, cx + 14, cy + 14, 3, lv_color_hex(0x1B2631));
}

static void draw_skip_button(lv_layer_t *layer, int cx, int cy, int dir, bool locked)
{
    draw_dot(layer, cx, cy, SO_SKIP_R, c_card());
    const lv_color_t ink = locked ? c_dim() : c_txt();
    const int pts[3][2] = { { cx - 8 * dir, cy - 12 }, { cx - 8 * dir, cy + 12 }, { cx + 8 * dir, cy } };
    draw_triangle(layer, pts, ink);
    const int bx = cx + 10 * dir;
    draw_rect(layer, bx - 2, cy - 12, bx + 2, cy + 12, 1, ink);
}

static void draw_player(lv_layer_t *layer)
{
    const sonos_now_t *n = &s_now;
    char buf[96];
    if (s_cover_ok) {
        draw_image(layer, &s_cover_img, SO_X, SO_TOP, SO_COVER);
    } else {
        draw_rect(layer, SO_X, SO_TOP, SO_X + SO_COVER - 1, SO_TOP + SO_COVER - 1, 8, c_card());
    }
    const int lh = lv_font_get_line_height(g_font_body);
    const int under = SO_TOP + SO_COVER + 10;
    if (n->source[0]) {
        snprintf(buf, sizeof(buf), "%s%.23s", strcmp(n->source, "Spotify") == 0   ? "Fra "
                                              : strcmp(n->source, "AirPlay") == 0 ? "Via "
                                                                                  : "",
                 n->source);
        draw_text_fit(layer, buf, SO_X, under, SO_COVER, c_dim());
    }
    if (s_note[0]) {
        draw_text_wrap(layer, s_note, SO_X, under + lh + 4, SO_COVER, 3, lv_palette_main(LV_PALETTE_RED));
    }
    if (n->empty) {
        return; /* the labels say so */
    }

    const int x1 = SO_COL_X, x2 = SO_COL_X + SO_COL_W - 1;
    if (n->duration_s > 0) {
        const int pos = position_now();
        draw_rect(layer, x1, SO_PROG_Y, x2, SO_PROG_Y + 5, 3, c_card());
        draw_rect(layer, x1, SO_PROG_Y, x1 + (int)((int64_t)(x2 - x1) * pos / n->duration_s), SO_PROG_Y + 5, 3,
                  c_txt());
        mmss(buf, sizeof(buf), pos);
        draw_text(layer, buf, x1, SO_PROG_Y + 12, SO_COL_W, LV_TEXT_ALIGN_LEFT, c_dim());
        mmss(buf, sizeof(buf), n->duration_s);
        draw_text(layer, buf, x1, SO_PROG_Y + 12, SO_COL_W, LV_TEXT_ALIGN_RIGHT, c_dim());
    }

    const int cx = SO_COL_X + SO_COL_W / 2;
    draw_skip_button(layer, cx - SO_SKIP_DX, SO_CTRL_Y, -1, !n->can_previous);
    if (playing() && !n->can_pause) {
        draw_stop_button(layer, cx, SO_CTRL_Y); /* a radio stops */
    } else {
        draw_play_button(layer, cx, SO_CTRL_Y, playing(), !(playing() ? n->can_pause : n->can_play));
    }
    draw_skip_button(layer, cx + SO_SKIP_DX, SO_CTRL_Y, 1, !n->can_next);

    if (n->volume >= 0) {
        const int mx = SO_COL_X + SO_VOL_R, plx = SO_COL_X + SO_COL_W - SO_VOL_R;
        draw_dot(layer, mx, SO_VOL_Y, SO_VOL_R, c_card());
        draw_rect(layer, mx - 9, SO_VOL_Y - 2, mx + 9, SO_VOL_Y + 2, 1, c_txt());
        draw_dot(layer, plx, SO_VOL_Y, SO_VOL_R, c_card());
        draw_rect(layer, plx - 9, SO_VOL_Y - 2, plx + 9, SO_VOL_Y + 2, 1, c_txt());
        draw_rect(layer, plx - 2, SO_VOL_Y - 9, plx + 2, SO_VOL_Y + 9, 1, c_txt());
        const int bx1 = mx + SO_VOL_R + 16, bx2 = plx - SO_VOL_R - 16;
        draw_rect(layer, bx1, SO_VOL_Y - 4, bx2, SO_VOL_Y + 3, 4, c_card());
        draw_rect(layer, bx1, SO_VOL_Y - 4, bx1 + (bx2 - bx1) * n->volume / 100, SO_VOL_Y + 3, 4, c_txt());
        snprintf(buf, sizeof(buf), "Volum %d %%", n->volume);
        draw_text(layer, buf, bx1, SO_VOL_Y + 8, bx2 - bx1, LV_TEXT_ALIGN_CENTER, c_dim());
    }
}

static void draw_picker(lv_layer_t *layer)
{
    for (int i = 0; i < s_tile_count; i++) {
        const tile_t *t = &s_tiles[i];
        lv_area_t a;
        tile_box(i, &a);
        if (!draw_visible(layer, a.x1, a.y1, a.x2, a.y2)) {
            continue;
        }
        if (t->ok) {
            draw_image(layer, &t->img, a.x1, a.y1, SO_TILE);
        } else {
            draw_rect(layer, a.x1, a.y1, a.x1 + SO_TILE - 1, a.y1 + SO_TILE - 1, 8, c_card());
        }
        if (!t->fav) { /* from Spotify: its green in a corner */
            draw_dot(layer, a.x1 + SO_TILE - 13, a.y1 + 13, 9, c_bg());
            draw_dot(layer, a.x1 + SO_TILE - 13, a.y1 + 13, 7, lv_color_hex(0x1DB954));
        }
        draw_text_wrap(layer, t->title[0] ? t->title : "Spilleliste", a.x1, a.y1 + SO_TILE + 6, SO_TILE, 2, c_txt());
    }
    /* A line at the bottom, below the second row. */
    const int lh = lv_font_get_line_height(g_font_body);
    const char *msg = s_note[0] ? s_note : s_list_note;
    if (msg[0]) {
        draw_text_fit(layer, msg, SO_X, BOARD_LCD_V_RES - lh - 4, BOARD_LCD_H_RES - 2 * SO_X,
                      s_note[0] ? lv_palette_main(LV_PALETTE_RED) : c_dim());
    }
}

static void sonos_draw_cb(lv_event_t *e)
{
    if (s_note[0] && lv_tick_get() - s_note_tick > SO_NOTE_MS) {
        s_note[0] = '\0';
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_row(layer);
    if (s_picker) {
        draw_picker(layer);
    } else if (s_have) {
        draw_player(layer);
    }
}

/* The labels for what's on show (adapter lock held). */
static void sonos_show(void)
{
    const bool player = !s_picker && s_have && !s_now.empty;
    lv_obj_t *const text[] = { s_track, s_artist, s_album };
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        if (player) {
            lv_obj_clear_flag(text[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(text[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (player) {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_track, s_now.title[0] ? s_now.title : "\xE2\x80\x93");
        lv_label_set_text(s_artist, s_now.artist);
        lv_label_set_text(s_album, s_now.album);
        lv_point_t sz;
        lv_text_get_size(&sz, lv_label_get_text(s_track), g_font_large, 0, 0, SO_COL_W, LV_TEXT_FLAG_NONE);
        const int lh = lv_font_get_line_height(g_font_large);
        const int h = sz.y > lh ? 2 * lh : lh;
        lv_obj_set_height(s_track, h);
        lv_obj_set_y(s_artist, SO_TOP + h + 8);
        lv_obj_set_y(s_album, SO_TOP + h + 12 + lv_font_get_line_height(g_font_body));
    } else if (!s_picker && s_have && s_now.empty && s_room >= 0) {
        lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        char buf[128];
        snprintf(buf, sizeof(buf), "Ingenting spilles i %s. Trykk Velg musikk for \xC3\xA5 starte noe.",
                 s_rooms[s_room].name);
        lv_label_set_text(s_msg, buf);
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(s_canvas);
}

static void sonos_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    big_clock_set(&s_clock, time(NULL));
    if (s_note[0] && lv_tick_get() - s_note_tick > SO_NOTE_MS) {
        lv_obj_invalidate(s_canvas);
    }
    if (s_picker || !s_have || !playing() || s_now.duration_s <= 0) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);
    a.x1 += SO_COL_X;
    a.x2 = a.x1 + SO_COL_W;
    a.y1 += SO_PROG_Y - 2;
    a.y2 = a.y1 + 12 + lv_font_get_line_height(g_font_body) + 4;
    lv_obj_invalidate_area(s_canvas, &a);
    if (!s_woke_at_end && s_now.position_s + (int)((lv_tick_get() - s_now_tick) / 1000) > s_now.duration_s + 1) {
        s_woke_at_end = true;
        app_wake_poll(); /* the next track */
    }
}

/* --------------------------------------------------------------------------
 * Touch (LVGL context)
 * ------------------------------------------------------------------------ */

static void send(cmd_kind_t kind, sonos_cmd_t cmd, int value)
{
    s_cmd.kind = kind;
    s_cmd.cmd = cmd;
    s_cmd.value = value;
    if (s_room >= 0) {
        s_cmd.target = s_rooms[lead_of(s_room)];
    }
    cmd_t *last = s_cmd_count ? &s_cmds[s_cmd_count - 1] : NULL;
    if (last && kind == CMD_SONOS && cmd == SONOS_VOLUME && last->kind == CMD_SONOS && last->cmd == SONOS_VOLUME &&
        strcmp(last->target.uuid, s_cmd.target.uuid) == 0) {
        *last = s_cmd; /* only the newest volume */
    } else {
        if (s_cmd_count == SO_CMDS) {
            memmove(s_cmds, s_cmds + 1, sizeof(s_cmds[0]) * (SO_CMDS - 1));
            s_cmd_count--;
        }
        s_cmds[s_cmd_count++] = s_cmd;
    }
    lv_obj_invalidate(s_canvas);
    app_wake_poll();
}

static bool near(int x, int y, int cx, int cy, int r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static bool in_box(int x, int y, const lv_area_t *a)
{
    return x >= a->x1 && x <= a->x2 && y >= a->y1 && y <= a->y2;
}

static void set_picker(bool on)
{
    s_picker = on;
    s_picker_shut = !on;
    note("");
    sonos_show();
}

bool sonos_tap(int x, int y)
{
    if (s_room_count > 0 && y >= SO_ROW_Y - 6 && y < SO_ROW_Y + SO_ROW_H + 6) {
        lv_area_t a;
        toggle_box(&a);
        a.y1 -= 6;
        a.y2 += 6;
        if (in_box(x, y, &a)) {
            set_picker(!s_picker);
            app_wake_poll(); /* the lists, or the room */
            return true;
        }
        for (int i = 0; i < s_room_count; i++) {
            if (chip_box(i, &a) && x >= a.x1 && x <= a.x2) {
                if (!s_picker && s_have && !s_now.empty && playing() && (in_group(i) || !s_busy[i])) {
                    /* Playing: the room joins, or leaves (and stops). */
                    if (in_group(i) && group_size() < 2) {
                        return true;
                    }
                    s_cmd.zone = s_rooms[i];
                    snprintf(s_cmd.coord, sizeof(s_cmd.coord), "%s", s_rooms[s_room].coord);
                    if (in_group(i)) {
                        if (i == s_room) { /* on show: the music, in the others */
                            for (int k = 0; k < s_room_count; k++) {
                                if (k != i && in_group(k)) {
                                    s_room = k;
                                    snprintf(s_room_name, sizeof(s_room_name), "%.47s", s_rooms[k].name);
                                    break;
                                }
                            }
                        }
                        snprintf(s_rooms[i].coord, sizeof(s_rooms[i].coord), "%s", s_rooms[i].uuid);
                        send(CMD_LEAVE, SONOS_PLAY, 0);
                    } else {
                        snprintf(s_rooms[i].coord, sizeof(s_rooms[i].coord), "%s", s_cmd.coord);
                        send(CMD_JOIN, SONOS_PLAY, 0);
                    }
                    return true;
                }
                if (i != s_room) {
                    const bool same = in_group(i);
                    s_room = i;
                    snprintf(s_room_name, sizeof(s_room_name), "%.47s", s_rooms[i].name);
                    s_have = same;
                    s_picker_shut = false;
                    note("");
                    sonos_show();
                    lv_label_set_text(g_status_label, "Henter fra Sonos...");
                    app_wake_poll();
                }
                return true;
            }
        }
        return false;
    }
    if (s_picker) {
        for (int i = 0; i < s_tile_count; i++) {
            lv_area_t a;
            tile_box(i, &a);
            if (in_box(x, y, &a)) {
                const tile_t *t = &s_tiles[i];
                s_cmd.fav = t->fav_idx;
                snprintf(s_cmd.uri, sizeof(s_cmd.uri), "%s", t->uri);
                set_picker(false);
                send(t->fav ? CMD_FAV : CMD_SPOTIFY, SONOS_PLAY, 0);
                return true;
            }
        }
        return false;
    }
    if (!s_have || s_now.empty) {
        return false;
    }
    const int cx = SO_COL_X + SO_COL_W / 2;
    if (near(x, y, cx, SO_CTRL_Y, SO_PLAY_R + 14)) {
        if (playing()) {
            send(CMD_SONOS, s_now.can_pause ? SONOS_PAUSE : SONOS_STOP, 0);
            s_now.position_s = position_now();
            s_now.state = SONOS_PAUSED;
        } else if (s_now.can_play) {
            send(CMD_SONOS, SONOS_PLAY, 0);
            s_now.state = SONOS_PLAYING;
        }
        s_now_tick = lv_tick_get();
        return true;
    }
    if (near(x, y, cx - SO_SKIP_DX, SO_CTRL_Y, SO_SKIP_R + 16)) {
        if (s_now.can_previous) {
            send(CMD_SONOS, SONOS_PREVIOUS, 0);
        }
        return true;
    }
    if (near(x, y, cx + SO_SKIP_DX, SO_CTRL_Y, SO_SKIP_R + 16)) {
        if (s_now.can_next) {
            send(CMD_SONOS, SONOS_NEXT, 0);
        }
        return true;
    }
    if (s_now.volume >= 0 && y > SO_VOL_Y - SO_VOL_R - 14 && y < SO_VOL_Y + SO_VOL_R + 14 && x >= SO_COL_X) {
        const int dir = x < SO_COL_X + SO_COL_W / 2 ? -1 : 1;
        int v = s_now.volume + dir * SO_VOL_STEP;
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        s_now.volume = v;
        send(CMD_SONOS, SONOS_VOLUME, v);
        return true;
    }
    /* Beside the buttons: nothing, rather than a change of screen. */
    return x >= SO_COL_X && y >= SO_PROG_Y - 20;
}

/* --------------------------------------------------------------------------
 * The weather task
 * ------------------------------------------------------------------------ */

static const char *err_text(esp_err_t err)
{
    switch (err) {
    case SONOS_ERR_NO_SPOTIFY:
        return "Sonos tok ikke imot Spotify. Legg til Spotify i Sonos-appen f\xC3\xB8rst.";
    case ESP_ERR_NOT_FOUND:
        return "Finner ingen Sonos-h\xC3\xB8yttalere p\xC3\xA5 nettverket.";
    default:
        return "Sonos svarte ikke. Pr\xC3\xB8v igjen.";
    }
}

/* A cover into px (allocated if NULL), SIDE square; whether it came. */
static bool fetch_picture(const char *url, uint16_t **px, int side)
{
    if (*px == NULL) {
        *px = heap_caps_aligned_alloc(16, (size_t)side * side * 2, MALLOC_CAP_SPIRAM);
        if (*px == NULL) {
            ESP_LOGW(TAG, "No memory for a cover");
            return false;
        }
    }
    unsigned char *pic = NULL;
    size_t len = 0;
    bool ok = false;
    if (sonos_get_image(url, &pic, &len) == ESP_OK) {
        ok = draw_picture_square(pic, len, *px, side);
        free(pic);
    }
    if (!ok) {
        ESP_LOGW(TAG, "Cover not shown: %.160s", url); /* not asked for again */
    }
    return ok;
}

/* The player's cover given back: not drawn under the picker, whose tiles
 * need the memory, nor on another screen. */
static void drop_cover(void)
{
    if (s_cover_px == NULL || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    heap_caps_free(s_cover_px);
    s_cover_px = NULL;
    s_cover_img.data = NULL;
    s_cover_ok = false;
    s_cover_url[0] = '\0';
    esp_lv_adapter_unlock();
}

/* The player's cover for `url`, if it isn't the one shown. Decoded
 * straight into the shown pixels: drawn as a plain square meanwhile. */
static void fetch_cover(const char *url)
{
    if (strcmp(url, s_cover_url) == 0) {
        return;
    }
    uint16_t *px = NULL;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_cover_ok = false;
        px = s_cover_px;
        s_cover_px = NULL; /* the weather task's while it decodes */
        esp_lv_adapter_unlock();
    }
    const bool ok = url[0] && fetch_picture(url, &px, SO_COVER);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_cover_px = px;
        s_cover_img.data = (const uint8_t *)px;
        s_cover_ok = ok && px != NULL;
        snprintf(s_cover_url, sizeof(s_cover_url), "%s", url);
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    } else {
        heap_caps_free(px);
    }
}

/* The tiles' covers still missing: up to `max` of them; true if more
 * remain. */
static bool fetch_tiles(int max)
{
    for (int i = 0; i < SO_TILES && max > 0; i++) {
        char art[SONOS_URL_MAX];
        uint16_t *px = NULL;
        bool want = false;
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return false;
        }
        tile_t *t = &s_tiles[i];
        if (i < s_tile_count && t->art[0] && strcmp(t->art, t->px_art) != 0) {
            want = true;
            snprintf(art, sizeof(art), "%s", t->art);
            px = t->px;
            t->px = NULL;
            t->ok = false;
        }
        esp_lv_adapter_unlock();
        if (!want) {
            continue;
        }
        max--;
        const bool ok = fetch_picture(art, &px, SO_TILE);
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            heap_caps_free(px);
            return false;
        }
        t->px = px;
        t->img.data = (const uint8_t *)px;
        t->ok = ok && px != NULL;
        snprintf(t->px_art, sizeof(t->px_art), "%s", art); /* not tried again if it failed */
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
    bool more = false;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        for (int i = 0; i < s_tile_count && !more; i++) {
            more = s_tiles[i].art[0] && strcmp(s_tiles[i].art, s_tiles[i].px_art) != 0;
        }
        esp_lv_adapter_unlock();
    }
    return more;
}

/* A playlist's name (and picture) from those remembered, or asked of
 * Spotify while `budget` lasts. */
static void name_playlist(spotify_recent_t *r, int *budget)
{
    for (int i = 0; i < SO_NAMES_MAX; i++) {
        if (strcmp(s_names[i].uri, r->uri) == 0) {
            snprintf(r->name, sizeof(r->name), "%s", s_names[i].name);
            if (s_names[i].image[0]) {
                snprintf(r->image_url, sizeof(r->image_url), "%s", s_names[i].image);
            }
            return;
        }
    }
    if (*budget <= 0) {
        return;
    }
    (*budget)--;
    char name[64] = "", image[160] = "";
    const esp_err_t err = spotify_get_playlist(r->uri, name, sizeof(name), image, sizeof(image));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return; /* asked again next time */
    }
    const int k = s_names_next++ % SO_NAMES_MAX;
    snprintf(s_names[k].uri, sizeof(s_names[k].uri), "%s", r->uri);
    snprintf(s_names[k].name, sizeof(s_names[k].name), "%s", name);
    snprintf(s_names[k].image, sizeof(s_names[k].image), "%s", image);
    s_names[k].missing = err == ESP_ERR_NOT_FOUND;
    snprintf(r->name, sizeof(r->name), "%s", name);
    if (image[0]) {
        snprintf(r->image_url, sizeof(r->image_url), "%s", image);
    }
}

/* The favourites and recently played, as the tiles; the note for the
 * picker. */
static void fetch_lists(const sonos_room_t *room)
{
    if (s_favs == NULL) {
        s_favs = heap_caps_calloc(SONOS_FAVS_MAX, sizeof(sonos_fav_t), MALLOC_CAP_SPIRAM);
        if (s_favs == NULL) {
            return;
        }
    }
    int favs = 0;
    if (sonos_get_favorites(room, s_favs, SONOS_FAVS_MAX, &favs) != ESP_OK) {
        favs = 0;
    }
    s_fav_count = favs;
    int recent = 0;
    const char *list_note = "";
    if (g_cfg->sp_client_id[0] == '\0' || spotify_login_state(NULL, 0) != SPOTIFY_CONNECTED) {
        list_note = "Koble skjermen til Spotify p\xC3\xA5 oppsettsiden for \xC3\xA5 se det du har spilt nylig.";
    } else {
        const esp_err_t err = spotify_get_recent(s_recent, SPOTIFY_RECENT_MAX, &recent);
        if (err == SPOTIFY_ERR_SCOPE) {
            list_note = "Logg inn p\xC3\xA5 Spotify p\xC3\xA5 nytt (oppsettsiden) for \xC3\xA5 se nylig spilt.";
        } else if (err != ESP_OK) {
            list_note = "Fikk ikke hentet det som er spilt nylig fra Spotify.";
        }
        int budget = SO_NAMES_PER_POLL;
        for (int i = 0; i < recent; i++) {
            if (s_recent[i].name[0] == '\0') {
                name_playlist(&s_recent[i], &budget);
            }
        }
        spotify_close(); /* asked again minutes from now: not kept open meanwhile */
    }
    if (favs == 0 && recent == 0 && list_note[0] == '\0') {
        list_note = "Ingen favoritter i Sonos-appen, og ingenting spilt nylig p\xC3\xA5 Spotify.";
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    /* Covers already fetched stay with the picture they show. */
    static EXT_RAM_BSS_ATTR tile_t old[SO_TILES];
    memcpy(old, s_tiles, sizeof(old));
    int n = 0;
    for (int i = 0; i < favs && n < SO_TILES; i++, n++) {
        tile_t *t = &s_tiles[n];
        t->fav = true;
        t->fav_idx = i;
        t->uri[0] = '\0';
        snprintf(t->title, sizeof(t->title), "%s", s_favs[i].title);
        snprintf(t->art, sizeof(t->art), "%s", s_favs[i].art_url);
    }
    for (int i = 0; i < recent && n < SO_TILES; i++, n++) {
        tile_t *t = &s_tiles[n];
        t->fav = false;
        t->fav_idx = -1;
        snprintf(t->uri, sizeof(t->uri), "%.63s", s_recent[i].uri);
        snprintf(t->title, sizeof(t->title), "%s", s_recent[i].name);
        snprintf(t->art, sizeof(t->art), "%s", s_recent[i].image_url);
    }
    /* The pixel buffers: one per tile, moved along with their picture. */
    for (int i = 0; i < SO_TILES; i++) {
        s_tiles[i].px = NULL;
        s_tiles[i].px_art[0] = '\0';
        s_tiles[i].ok = false;
    }
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < SO_TILES; k++) {
            if (old[k].px && old[k].px_art[0] && strcmp(old[k].px_art, s_tiles[i].art) == 0) {
                s_tiles[i].px = old[k].px;
                snprintf(s_tiles[i].px_art, sizeof(s_tiles[i].px_art), "%s", old[k].px_art);
                s_tiles[i].ok = old[k].ok;
                old[k].px = NULL;
                break;
            }
        }
    }
    for (int i = 0, k = 0; i < SO_TILES; i++) {
        if (s_tiles[i].px == NULL) {
            /* An unused buffer, if any, for the next picture. */
            while (k < SO_TILES && old[k].px == NULL) {
                k++;
            }
            if (k < SO_TILES) {
                s_tiles[i].px = old[k].px;
                old[k++].px = NULL;
            }
        }
        s_tiles[i].img.data = (const uint8_t *)s_tiles[i].px;
    }
    for (int k = 0; k < SO_TILES; k++) {
        heap_caps_free(old[k].px); /* any not needed now */
    }
    s_tile_count = n;
    snprintf(s_list_note, sizeof(s_list_note), "%s", list_note);
    lv_obj_invalidate(s_canvas);
    esp_lv_adapter_unlock();
    stamp_now(&s_lists_at);
}

void sonos_release(void)
{
    sonos_client_release();
    drop_cover();
    /* The pointers are only set by this task: nothing to do, no lock. */
    bool any = false;
    for (int i = 0; i < SO_TILES; i++) {
        any |= s_tiles[i].px != NULL;
    }
    if (!any || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    for (int i = 0; i < SO_TILES; i++) {
        heap_caps_free(s_tiles[i].px);
        s_tiles[i].px = NULL;
        s_tiles[i].img.data = NULL;
        s_tiles[i].ok = false;
        s_tiles[i].px_art[0] = '\0';
    }
    esp_lv_adapter_unlock();
}

/* The room to show from those found: the one chosen, else the setup
 * page's, else the first playing, else the first (adapter lock held). */
static int pick_room(const sonos_room_t *rooms, int n, const bool *busy)
{
    for (int pass = 0; pass < 3; pass++) {
        const char *want = pass == 0 ? s_room_name : pass == 1 ? g_cfg->so_room : NULL;
        for (int i = 0; i < n; i++) {
            if (pass < 2 ? want[0] && strcmp(rooms[i].name, want) == 0 : busy[i]) {
                return i;
            }
        }
    }
    return n > 0 ? 0 : -1;
}

/* The rooms, asked again now and then. False if there are none. */
static bool refresh_rooms(int for_view)
{
    if (s_room_count > 0 && !s_rediscover && stamp_fresh(&s_rooms_at, SO_ROOMS_MS)) {
        return true;
    }
    int n = 0;
    const esp_err_t err = sonos_get_rooms(s_room_scratch, SONOS_ROOMS_MAX, &n);
    if (err != ESP_OK || n == 0) {
        diag_fail(DIAG_SONOS, err != ESP_OK ? err : ESP_ERR_NOT_FOUND);
        backoff_fail(&s_fail);
        if (lock_for_view(for_view)) {
            if (s_room_count == 0) {
                lv_label_set_text(g_status_label, err_text(ESP_ERR_NOT_FOUND));
            }
            esp_lv_adapter_unlock();
        }
        return s_room_count > 0;
    }
    s_rediscover = false;
    stamp_now(&s_rooms_at);
    /* Which play, the first time, to start on one of them (unless the setup
     * page names one): each group's leader asked. */
    bool busy[SONOS_ROOMS_MAX] = { 0 };
    bool named = false;
    for (int i = 0; i < n; i++) {
        named |= strcmp(s_room_scratch[i].name, g_cfg->so_room) == 0;
    }
    for (int i = 0; i < n && s_room_name[0] == '\0' && !named; i++) {
        const sonos_room_t *r = &s_room_scratch[i];
        sonos_state_t st;
        if (strcmp(r->uuid, r->coord) == 0 && sonos_get_state(r, &st) == ESP_OK && st == SONOS_PLAYING) {
            for (int k = 0; k < n; k++) {
                busy[k] |= strcmp(s_room_scratch[k].coord, r->uuid) == 0;
            }
        }
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        memcpy(s_rooms, s_room_scratch, sizeof(s_rooms));
        s_room_count = n;
        const int r = pick_room(s_rooms, n, busy);
        if (r >= 0 && strcmp(s_rooms[r].name, s_room_name) != 0) {
            ESP_LOGI(TAG, "Showing %s (of %d rooms)", s_rooms[r].name, n);
            s_have = false;
        }
        s_room = r;
        if (r >= 0) {
            snprintf(s_room_name, sizeof(s_room_name), "%.47s", s_rooms[r].name);
        }
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
    return n > 0;
}

uint32_t sonos_poll(int for_view)
{
    if (!refresh_rooms(for_view)) {
        return backoff_ms(&s_fail, SO_RETRY_MS);
    }
    /* The buttons pressed, in turn. */
    static EXT_RAM_BSS_ATTR cmd_t cmds[SO_CMDS];
    int ncmds = 0;
    bool picker = false;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return SO_IDLE_MS;
    }
    ncmds = s_cmd_count;
    memcpy(cmds, s_cmds, sizeof(cmds[0]) * (size_t)ncmds);
    s_cmd_count = 0;
    esp_lv_adapter_unlock();

    if (ncmds == 0 && backoff_waiting(&s_fail, SO_RETRY_MS)) {
        return backoff_ms(&s_fail, SO_RETRY_MS);
    }
    for (int i = 0; i < ncmds; i++) {
        const cmd_t *c = &cmds[i];
        esp_err_t err;
        if (c->kind == CMD_FAV) {
            err = c->fav >= 0 && c->fav < s_fav_count ? sonos_play_favorite(&c->target, &s_favs[c->fav])
                                                      : ESP_ERR_INVALID_ARG;
        } else if (c->kind == CMD_SPOTIFY) {
            err = sonos_play_spotify(&c->target, c->uri);
        } else if (c->kind == CMD_JOIN) {
            err = sonos_join(&c->zone, c->coord);
        } else if (c->kind == CMD_LEAVE) {
            err = sonos_leave(&c->zone);
        } else {
            err = sonos_command(&c->target, c->cmd, c->value);
        }
        if (err != ESP_OK) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                note(err_text(err));
                lv_obj_invalidate(s_canvas);
                esp_lv_adapter_unlock();
            }
        } else if (i == ncmds - 1) {
            vTaskDelay(pdMS_TO_TICKS(c->kind == CMD_SONOS ? SO_SETTLE_MS : 3 * SO_SETTLE_MS));
        }
        if (c->kind == CMD_JOIN || c->kind == CMD_LEAVE) {
            s_rediscover = true; /* the groups as they are now */
        }
    }
    if (s_rediscover && ncmds > 0) {
        refresh_rooms(for_view);
    }

    /* The group on show, by its leader. */
    sonos_room_t room;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return SO_IDLE_MS;
    }
    if (s_room < 0 || s_room >= s_room_count) {
        esp_lv_adapter_unlock();
        return SO_IDLE_MS;
    }
    room = s_rooms[lead_of(s_room)];
    picker = s_picker;
    esp_lv_adapter_unlock();

    esp_err_t err = sonos_get_now(&room, &s_now_scratch);
    if (err != ESP_OK) {
        diag_fail(DIAG_SONOS, err);
        backoff_fail(&s_fail);
        s_rediscover = true; /* moved to another address, perhaps */
        if (lock_for_view(for_view)) {
            if (s_have) {
                note(err_text(err));
                lv_obj_invalidate(s_canvas);
            } else {
                lv_label_set_text(g_status_label, "F\xC3\xA5r ikke kontakt med Sonos. Pr\xC3\xB8ver igjen...");
            }
            esp_lv_adapter_unlock();
        }
        return backoff_ms(&s_fail, SO_RETRY_MS);
    }
    diag_ok(DIAG_SONOS);
    backoff_ok(&s_fail);

    /* Whether the other groups play: their rooms join no one by a tap. */
    bool busy[SONOS_ROOMS_MAX] = { 0 };
    int count = 0;
    static EXT_RAM_BSS_ATTR sonos_room_t others[SONOS_ROOMS_MAX];
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        count = s_room_count;
        memcpy(others, s_rooms, sizeof(others));
        esp_lv_adapter_unlock();
    }
    for (int i = 0; i < count; i++) {
        sonos_state_t st;
        if (strcmp(others[i].uuid, others[i].coord) == 0 && strcmp(others[i].uuid, room.uuid) != 0 &&
            sonos_get_state(&others[i], &st) == ESP_OK && (st == SONOS_PLAYING || st == SONOS_TRANSITIONING)) {
            for (int k = 0; k < count; k++) {
                busy[k] |= strcmp(others[k].coord, others[i].uuid) == 0;
            }
        }
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (count == s_room_count && memcmp(busy, s_busy, sizeof(busy)) != 0) {
            memcpy(s_busy, busy, sizeof(busy));
            lv_obj_invalidate(s_canvas);
        }
        esp_lv_adapter_unlock();
    }

    /* An empty room opens the picker, unless closed for it. */
    bool opened = false;
    if (s_now_scratch.empty && esp_lv_adapter_lock(-1) == ESP_OK) {
        if (!s_picker && !s_picker_shut) {
            s_picker = opened = true;
        }
        picker = s_picker;
        esp_lv_adapter_unlock();
    }
    bool more = false;
    if (picker) {
        drop_cover();
        if (!stamp_fresh(&s_lists_at, SO_LISTS_MS)) {
            fetch_lists(&room);
        }
        more = fetch_tiles(3);
    } else {
        fetch_cover(s_now_scratch.art_url);
    }

    bool changed = opened;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_room >= 0 && strcmp(s_rooms[lead_of(s_room)].uuid, room.uuid) == 0) { /* not changed meanwhile */
            if (strcmp(s_now_scratch.title, s_now.title) != 0 || s_now_scratch.state != SONOS_PLAYING) {
                s_woke_at_end = false;
            }
            changed |= !s_have || now_changed(&s_now, &s_now_scratch);
            s_now = s_now_scratch;
            s_now_tick = lv_tick_get();
            s_have = true;
        }
        esp_lv_adapter_unlock();
    }
    if (lock_for_view(for_view)) {
        lv_label_set_text(g_status_label, s_have ? "" : "Henter fra Sonos...");
        if (changed) {
            sonos_show(); /* not every few seconds: a redraw of the whole screen */
        }
        esp_lv_adapter_unlock();
    }
    if (more) {
        return 100; /* the rest of the covers */
    }
    return s_now_scratch.state == SONOS_PLAYING ? SO_PLAYING_MS : SO_IDLE_MS;
}

/* --------------------------------------------------------------------------
 * Building
 * ------------------------------------------------------------------------ */

static lv_obj_t *text_label(const lv_font_t *font, lv_color_t colour, int y)
{
    lv_obj_t *l = lv_label_create(s_root);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_pos(l, SO_COL_X, y);
    lv_obj_set_width(l, SO_COL_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font));
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

lv_obj_t *sonos_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    if (g_cfg->so_dark && g_cfg->theme != APP_THEME_DARK) {
        /* Dark on its own: the dark theme's background and text. */
        lv_obj_set_style_bg_color(s_root, lv_color_hex(0x15171A), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_root, lv_palette_lighten(LV_PALETTE_GREY, 5), 0);
    }
    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, sonos_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, SO_X, 4);
    lv_label_set_text(s_title, "Sonos");
    big_clock_create(&s_clock, s_root, SO_X);

    s_track = text_label(g_font_large, c_txt(), SO_TOP);
    s_artist = text_label(g_font_body, c_txt(), SO_TOP + lv_font_get_line_height(g_font_large) + 8);
    s_album = text_label(g_font_body, c_dim(), SO_TOP + 2 * lv_font_get_line_height(g_font_large));
    s_msg = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_msg, g_font_large, 0);
    lv_obj_set_style_text_color(s_msg, c_txt(), 0);
    lv_obj_set_pos(s_msg, SO_COL_X, SO_TOP);
    lv_obj_set_width(s_msg, SO_COL_W);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);

    /* The pixels come with the pictures (fetch_picture). */
    s_cover_img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = SO_COVER, .h = SO_COVER,
                    .stride = SO_COVER * 2 },
        .data_size = SO_COVER * SO_COVER * 2,
    };
    for (int i = 0; i < SO_TILES; i++) {
        s_tiles[i].img = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = SO_TILE, .h = SO_TILE,
                        .stride = SO_TILE * 2 },
            .data_size = SO_TILE * SO_TILE * 2,
        };
    }
    lv_timer_create(sonos_tick_cb, SO_TICK_MS, NULL);
    return s_root;
}

void sonos_enter(void)
{
    big_clock_set(&s_clock, time(NULL));
    note("");
    s_picker_shut = false;
    lv_label_set_text(g_status_label, s_room_count == 0 ? "Leter etter Sonos-h\xC3\xB8yttalere..."
                                      : s_have        ? ""
                                                      : "Henter fra Sonos...");
    sonos_show();
}
