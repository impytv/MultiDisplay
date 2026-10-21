/* The Spotify screen: what the display's Spotify account is playing - the
 * cover, the title, the artists and the album, how far it has got - with
 * buttons for the previous track, play/pause, the next track, shuffle,
 * repeat and the volume, and the device it plays on; a tap on the progress
 * bar winds there. With nothing playing, a button per device of the
 * account starts playback there. "Velg musikk" swaps the player for a grid
 * of what the account played lately and its playlists, and a tap starts
 * one on the device playing (or the one that played last). The display plays nothing
 * itself: it controls Spotify on a phone, speaker or computer, which needs
 * Spotify Premium. Logging in is on the setup page (spotify_client.h).
 *
 * One screen for the display. While on show, Spotify is asked every few
 * seconds (SP_PLAYING_MS while playing), and at once after a button; the
 * progress bar moves on by itself between. Like the month calendar's, the
 * buttons are only drawn: the tap layer hands touches here (spotify_tap)
 * first, and the weather task sends the command. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "diag.h"
#include "draw.h"
#include "form_util.h"
#include "music_tiles.h"
#include "spotify.h"
#include "spotify_client.h"
#include "waveshare_rgb_lcd_port.h"
#include "wifi_provision.h"

static const char *TAG = "spotify_screen";

#define SP_X          12
#define SP_TOP        64
#define SP_COVER      SPOTIFY_COVER_PX
#define SP_COL_X      (SP_X + SP_COVER + 28) /* the column right of the cover */
#define SP_COL_W      (BOARD_LCD_H_RES - SP_COL_X - SP_X)
#define SP_PROG_Y     250 /* the progress bar */
#define SP_CTRL_Y     330 /* the middle of the playback buttons */
#define SP_PLAY_R     38
#define SP_SKIP_R     30
#define SP_SKIP_DX    115 /* from the play button to the others */
#define SP_MODE_DX    190 /* shuffle and repeat */
#define SP_MODE_R     22
#define SP_PICK_Y     8   /* "Velg musikk", beside the title */
#define SP_PICK_H     44
#define SP_PICK_PAD   18
#define SP_VOL_Y      416 /* the middle of the volume row */
#define SP_VOL_R      24
#define SP_VOL_STEP   10
#define SP_DEV_Y      (SP_TOP + 100) /* the device buttons, with nothing playing */
#define SP_DEV_H      64
#define SP_DEV_GAP    12
#define SP_PLAYING_MS 5000
#define SP_PAUSED_MS  10000
#define SP_IDLE_MS    30000   /* not logged in */
#define SP_RETRY_MS   15000
#define SP_SETTLE_MS  400     /* after a command, before asking what plays */
#define SP_NOTE_MS    10000   /* a message under the cover stays this long */
#define SP_TICK_MS    500
#define SP_COVER_RETRY_MS 30000 /* a cover that failed, asked for again */
#define SP_LISTS_MS   (2 * 60 * 1000) /* the picker's lists, asked again */
#define SP_NAMES_PER_POLL 4
#define SP_TAB_Y      58      /* the picker's pages, under the title */
#define SP_TAB_H      44
#define SP_GRID_TOP   118     /* the picker's tiles, under them */
#define SP_QUEUE_MS   15000   /* "Kø" asked again while shown (and as the track changes) */
#define SP_PEND_MS    10000   /* "Starter ..." at most this long */
#define SP_PEND_POLL_MS 1500  /* meanwhile asked this often */
#define SP_PICKER_IDLE_MS (2 * 60 * 1000) /* the picker closed after this without a touch */

static lv_obj_t *s_root, *s_canvas, *s_title, *s_track, *s_artist, *s_album, *s_msg;
static big_clock_t s_clock;

/* What plays and the devices, under the adapter lock; written by the
 * weather task. */
static spotify_player_t s_player;
static bool s_have;            /* s_player holds an answer */
static uint32_t s_player_tick; /* lv_tick_get() when it came */
static spotify_device_t s_devices[SPOTIFY_DEVICES_MAX];
static int s_dev_count;
static char s_note[96];        /* a message under the cover, "" = none */
static uint32_t s_note_tick;
static bool s_woke_at_end;     /* the poll woken as the track ended */
/* A device that refused a command as restricted, though Spotify didn't
 * mark it so: its controls are greyed out too. */
static char s_refused[48];
static char s_last_device[48]; /* the one that played last, for the picker */
static bool s_picker;          /* the picker on show, not the player; set by taps */
static uint32_t s_touch_tick;  /* the last tap: the picker closes some minutes after */
/* The picker's pages, as tabs under the title. */
typedef enum { PG_RECENT, PG_LISTS, PG_QUEUE, PG_COUNT } page_t;
static const char *const PAGE_NAMES[PG_COUNT] = { "Nylig", "Lister", "K\xC3\xB8" };
static page_t s_page;          /* set by taps */
static char s_page_note[PG_COUNT][160]; /* each page's message, "" = none */
/* The tiles of the page shown (what was played lately, or the playlists),
 * started by their URI. */
static EXT_RAM_BSS_ATTR mgrid_t s_grid;
/* "Kø": what plays, then what comes. */
static EXT_RAM_BSS_ATTR spotify_track_t s_queue[MT_LIST_MAX];
static int s_queue_count;
/* "Starter ...": what was tapped, shown at once till Spotify plays it (or
 * SP_PEND_MS). */
static struct {
    bool on;
    char title[64];
    char where[80];       /* "Starter på Stue-TV ..." */
    char uri[64];         /* the context started, "" for a skip in "Kø" */
    char before_item[48]; /* what played as it was tapped */
    uint32_t tick;
    mpic_t pic;
} s_pend;

/* The cover, SP_COVER square, RGB565 (PSRAM, only while the screen is on
 * show); s_cover_url is the one in it. The weather task decodes straight
 * into it, taken out from under the screen meanwhile (a plain square for a
 * moment): a second 180 KB block to decode into, for the old cover to stay
 * till then, was often not to be had in one piece. */
static uint16_t *s_cover_px;
static lv_image_dsc_t s_cover_img;
static char s_cover_url[160];
static bool s_cover_ok;

/* A button pressed, for the weather task to send (under the lock). */
static struct {
    bool pending;
    spotify_cmd_t cmd;
    int value;
    char device[48];
    char uri[64]; /* a tile: started instead of cmd */
} s_cmd;

static spotify_player_t s_scratch; /* the weather task's */
static spotify_device_t s_dev_scratch[SPOTIFY_DEVICES_MAX];
static backoff_t s_fail;
static fetch_stamp_t s_recent_at, s_lists_at, s_queue_at;
static EXT_RAM_BSS_ATTR spotify_recent_t s_recent[MT_MAX], s_lists[MT_MAX];
static int s_recent_count, s_lists_count;
static int s_built = -1;          /* the page in s_grid, -1 = none */
static char s_queue_item[48];     /* the track "Kø" was asked for */

static bool dark(void)
{
    return g_cfg->theme == APP_THEME_DARK || g_cfg->sp_dark;
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

static lv_color_t c_green(void)
{
    return lv_color_hex(0x1DB954); /* Spotify's */
}

/* "3:07" */
static void mmss(char *out, size_t n, int ms)
{
    const int s = ms / 1000;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/* Where the playback has got to now, from the last answer. */
static int progress_now(void)
{
    int p = s_player.progress_ms;
    if (s_player.playing) {
        p += (int)(lv_tick_get() - s_player_tick);
    }
    return s_player.duration_ms > 0 && p > s_player.duration_ms ? s_player.duration_ms : p;
}

static bool showing_player(void)
{
    return s_have && (s_player.active || s_pend.on) && !s_picker;
}

static const char *pick_text(void)
{
    return s_picker ? "Spiller n\xC3\xA5" : "Velg musikk";
}

/* "Velg musikk", beside the title. */
static void pick_box(lv_area_t *a)
{
    lv_point_t sz;
    lv_text_get_size(&sz, "Spotify", g_font_large, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    a->x1 = SP_X + sz.x + 24;
    a->x2 = a->x1 + draw_text_w("Velg musikk") + 2 * SP_PICK_PAD - 1;
    a->y1 = SP_PICK_Y;
    a->y2 = SP_PICK_Y + SP_PICK_H - 1;
}

/* The device buttons with nothing playing: button i's box. */
static void dev_box(int i, lv_area_t *a)
{
    const int w = (BOARD_LCD_H_RES - 2 * SP_X - SP_DEV_GAP) / 2;
    a->x1 = SP_X + (i % 2) * (w + SP_DEV_GAP);
    a->y1 = SP_DEV_Y + (i / 2) * (SP_DEV_H + SP_DEV_GAP);
    a->x2 = a->x1 + w - 1;
    a->y2 = a->y1 + SP_DEV_H - 1;
}

/* How many device buttons fit above the bottom. */
static int dev_rows_max(void)
{
    return (BOARD_LCD_V_RES - 8 - SP_DEV_Y + SP_DEV_GAP) / (SP_DEV_H + SP_DEV_GAP);
}

/* --------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

/* The playing device takes no commands from here (adapter lock held). */
static bool device_locked(void)
{
    const spotify_device_t *d = &s_player.device;
    return d->restricted || (s_refused[0] && strcmp(s_refused, d->id) == 0);
}

static void draw_play_button(lv_layer_t *layer, int cx, int cy, bool playing, bool locked)
{
    draw_dot(layer, cx, cy, SP_PLAY_R, locked ? c_card() : c_green());
    const lv_color_t ink = locked ? c_dim() : lv_color_hex(0x101820);
    if (playing) { /* pause: two bars */
        draw_rect(layer, cx - 13, cy - 15, cx - 5, cy + 15, 2, ink);
        draw_rect(layer, cx + 5, cy - 15, cx + 13, cy + 15, 2, ink);
    } else {
        const int pts[3][2] = { { cx - 10, cy - 17 }, { cx - 10, cy + 17 }, { cx + 18, cy } };
        draw_triangle(layer, pts, ink);
    }
}

/* Previous (dir -1) or next (+1): a triangle against a bar. */
static void draw_skip_button(lv_layer_t *layer, int cx, int cy, int dir, bool locked)
{
    draw_dot(layer, cx, cy, SP_SKIP_R, c_card());
    const lv_color_t ink = locked ? c_dim() : c_txt();
    const int pts[3][2] = { { cx - 8 * dir, cy - 12 }, { cx - 8 * dir, cy + 12 }, { cx + 8 * dir, cy } };
    draw_triangle(layer, pts, ink);
    const int bx = cx + 10 * dir;
    draw_rect(layer, bx - 2, cy - 12, bx + 2, cy + 12, 1, ink);
}

static void draw_player(lv_layer_t *layer)
{
    const spotify_player_t *p = &s_player;
    char buf[96];
    if (s_pend.on) { /* what was tapped, till it plays */
        mpic_draw(layer, &s_pend.pic, SP_X, SP_TOP, SP_COVER, c_card(), c_card());
        return;
    }
    /* The cover, or a plain square until it's in. */
    if (s_cover_ok) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &s_cover_img;
        lv_area_t a = { SP_X, SP_TOP, SP_X + SP_COVER - 1, SP_TOP + SP_COVER - 1 };
        lv_draw_image(layer, &d, &a);
    } else {
        draw_rect(layer, SP_X, SP_TOP, SP_X + SP_COVER - 1, SP_TOP + SP_COVER - 1, 8, c_card());
    }
    const int lh = lv_font_get_line_height(g_font_body);
    /* Under it, the device, and a message if any. */
    if (p->device.name[0]) {
        snprintf(buf, sizeof(buf), "%s %s", p->playing ? "Spiller p\xC3\xA5" : "Pauset p\xC3\xA5", p->device.name);
        draw_text_fit(layer, buf, SP_X, SP_TOP + SP_COVER + 10, SP_COVER, c_dim());
    }
    const bool locked = device_locked();
    if (s_note[0] || locked) {
        draw_text_wrap(layer, s_note[0] ? s_note : "Spotify lar ikke skjermen styre denne enheten.", SP_X,
                       SP_TOP + SP_COVER + 14 + lh, SP_COVER, 2,
                       s_note[0] ? lv_palette_main(LV_PALETTE_RED) : c_dim());
    }

    /* Progress, and the times either side under it. */
    const int x1 = SP_COL_X, x2 = SP_COL_X + SP_COL_W - 1;
    draw_rect(layer, x1, SP_PROG_Y, x2, SP_PROG_Y + 5, 3, c_card());
    const int pos = progress_now();
    if (p->duration_ms > 0) {
        const int px = x1 + (int)((int64_t)(x2 - x1) * pos / p->duration_ms);
        draw_rect(layer, x1, SP_PROG_Y, px, SP_PROG_Y + 5, 3, c_txt());
        if (p->can_seek && !locked) { /* a knob: it can be wound */
            draw_dot(layer, px, SP_PROG_Y + 2, 8, c_txt());
        }
        mmss(buf, sizeof(buf), pos);
        draw_text(layer, buf, x1, SP_PROG_Y + 12, SP_COL_W, LV_TEXT_ALIGN_LEFT, c_dim());
        mmss(buf, sizeof(buf), p->duration_ms);
        draw_text(layer, buf, x1, SP_PROG_Y + 12, SP_COL_W, LV_TEXT_ALIGN_RIGHT, c_dim());
    }

    const int cx = SP_COL_X + SP_COL_W / 2;
    draw_skip_button(layer, cx - SP_SKIP_DX, SP_CTRL_Y, -1, locked);
    draw_play_button(layer, cx, SP_CTRL_Y, p->playing, locked);
    draw_skip_button(layer, cx + SP_SKIP_DX, SP_CTRL_Y, 1, locked);
    draw_mode_button(layer, LV_SYMBOL_SHUFFLE, cx - SP_MODE_DX, SP_CTRL_Y, SP_MODE_R, p->shuffle, false,
                     locked || !p->can_shuffle, c_card(), c_txt(), c_dim(), c_green());
    draw_mode_button(layer, LV_SYMBOL_LOOP, cx + SP_MODE_DX, SP_CTRL_Y, SP_MODE_R, p->repeat != SPOTIFY_REPEAT_OFF,
                     p->repeat == SPOTIFY_REPEAT_TRACK, locked || !p->can_repeat, c_card(), c_txt(), c_dim(),
                     c_green());

    /* Volume: - [bar] +, the percent under the bar. */
    if (p->device.has_volume && p->device.volume >= 0 && !locked) {
        const int mx = SP_COL_X + SP_VOL_R, plx = SP_COL_X + SP_COL_W - SP_VOL_R;
        draw_dot(layer, mx, SP_VOL_Y, SP_VOL_R, c_card());
        draw_rect(layer, mx - 10, SP_VOL_Y - 2, mx + 10, SP_VOL_Y + 2, 1, c_txt());
        draw_dot(layer, plx, SP_VOL_Y, SP_VOL_R, c_card());
        draw_rect(layer, plx - 10, SP_VOL_Y - 2, plx + 10, SP_VOL_Y + 2, 1, c_txt());
        draw_rect(layer, plx - 2, SP_VOL_Y - 10, plx + 2, SP_VOL_Y + 10, 1, c_txt());
        const int bx1 = mx + SP_VOL_R + 16, bx2 = plx - SP_VOL_R - 16;
        draw_rect(layer, bx1, SP_VOL_Y - 4, bx2, SP_VOL_Y + 3, 4, c_card());
        draw_rect(layer, bx1, SP_VOL_Y - 4, bx1 + (bx2 - bx1) * p->device.volume / 100, SP_VOL_Y + 3, 4, c_txt());
        snprintf(buf, sizeof(buf), "Volum %d %%", p->device.volume);
        draw_text(layer, buf, bx1, SP_VOL_Y + 10, bx2 - bx1, LV_TEXT_ALIGN_CENTER, c_dim());
    }
}

static void draw_devices(lv_layer_t *layer)
{
    const int lh = lv_font_get_line_height(g_font_body);
    const int n = s_dev_count < 2 * dev_rows_max() ? s_dev_count : 2 * dev_rows_max();
    for (int i = 0; i < n; i++) {
        lv_area_t a;
        dev_box(i, &a);
        draw_rect(layer, a.x1, a.y1, a.x2, a.y2, 10, c_card());
        const int pts[3][2] = { { a.x1 + 20, a.y1 + SP_DEV_H / 2 - 11 },
                                { a.x1 + 20, a.y1 + SP_DEV_H / 2 + 11 },
                                { a.x1 + 39, a.y1 + SP_DEV_H / 2 } };
        draw_triangle(layer, pts, c_green());
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%s%s", s_devices[i].name, s_devices[i].type[0] ? " \xC2\xB7 " : "",
                 s_devices[i].type);
        draw_text_fit(layer, buf, a.x1 + 56, a.y1 + (SP_DEV_H - lh) / 2, a.x2 - a.x1 - 68,
                      s_devices[i].restricted ? c_dim() : c_txt());
    }
    if (s_note[0]) {
        draw_text_wrap(layer, s_note, SP_X, BOARD_LCD_V_RES - 2 * lh - 8, BOARD_LCD_H_RES - 2 * SP_X, 2,
                       lv_palette_main(LV_PALETTE_RED));
    }
}

static void draw_picker(lv_layer_t *layer)
{
    const lv_color_t bg = dark() ? lv_color_hex(0x15171A) : lv_color_white();
    mtabs_draw(layer, PAGE_NAMES, PG_COUNT, s_page, SP_X, SP_TAB_Y, SP_TAB_H, c_card(), c_txt(), c_txt(), bg);
    if (s_page == PG_QUEUE) {
        for (int i = 0; i < s_queue_count && i < MT_LIST_MAX; i++) {
            mlist_draw_item(layer, SP_GRID_TOP, i, s_queue[i].title, s_queue[i].artist, s_player.active && i == 0,
                            c_card(), c_txt(), c_dim(), c_green(), lv_color_hex(0x101820));
        }
    } else {
        mgrid_draw(layer, &s_grid, c_card(), bg, c_txt(), c_card(), c_green());
    }
    const int lh = lv_font_get_line_height(g_font_body);
    const bool loading = s_page != PG_QUEUE && s_grid.count == 0 && s_built != (int)s_page;
    const char *msg = s_note[0]                ? s_note
                      : s_page_note[s_page][0] ? s_page_note[s_page]
                      : loading                ? "Henter \xE2\x80\xA6" /* till the lists come */
                                               : "";
    if (msg[0]) {
        draw_text_fit(layer, msg, SP_X, BOARD_LCD_V_RES - lh - 4, BOARD_LCD_H_RES - 2 * SP_X,
                      s_note[0] ? lv_palette_main(LV_PALETTE_RED) : c_dim());
    }
}

static void spotify_draw_cb(lv_event_t *e)
{
    if (s_note[0] && lv_tick_get() - s_note_tick > SP_NOTE_MS) {
        s_note[0] = '\0';
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_have) {
        lv_area_t a;
        pick_box(&a);
        draw_rect(layer, a.x1, a.y1, a.x2, a.y2, SP_PICK_H / 2, c_green());
        const int lh = lv_font_get_line_height(g_font_body);
        draw_text(layer, pick_text(), a.x1, a.y1 + (SP_PICK_H - lh) / 2, a.x2 - a.x1 + 1, LV_TEXT_ALIGN_CENTER,
                  lv_color_hex(0x101820));
    }
    if (s_picker) {
        draw_picker(layer);
    } else if (showing_player()) {
        draw_player(layer);
    } else if (s_have) {
        draw_devices(layer);
    }
}

/* The labels for what s_player and s_devices hold (adapter lock held). */
static void spotify_show(void)
{
    const bool player = showing_player();
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
        const bool ad = strcmp(s_player.kind, "ad") == 0;
        if (s_pend.on) {
            lv_label_set_text(s_track, s_pend.title[0] ? s_pend.title : "\xE2\x80\x93");
            lv_label_set_text(s_artist, s_pend.where);
            lv_label_set_text(s_album, "");
        } else {
            lv_label_set_text(s_track, ad ? "Reklame" : s_player.title[0] ? s_player.title : "\xE2\x80\x93");
            lv_label_set_text(s_artist, s_player.artist);
            lv_label_set_text(s_album, s_player.album);
        }
        /* The title over at most two lines, the others under it. */
        lv_point_t sz;
        lv_text_get_size(&sz, lv_label_get_text(s_track), g_font_large, 0, 0, SP_COL_W, LV_TEXT_FLAG_NONE);
        const int lh = lv_font_get_line_height(g_font_large);
        const int h = sz.y > lh ? 2 * lh : lh;
        lv_obj_set_height(s_track, h);
        lv_obj_set_y(s_artist, SP_TOP + h + 8);
        lv_obj_set_y(s_album, SP_TOP + h + 12 + lv_font_get_line_height(g_font_body));
    } else if (s_have && !s_picker) {
        lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_msg, s_dev_count > 0
                                     ? "Ingenting spilles n\xC3\xA5. Trykk for \xC3\xA5 spille p\xC3\xA5:"
                                     : "Ingenting spilles n\xC3\xA5, og ingen Spotify-enheter er p\xC3\xA5. "
                                       "\xC3\x85pne Spotify p\xC3\xA5 telefonen, PC-en eller en h\xC3\xB8yttaler.");
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(s_canvas);
}

/* The screen's message for the login, "" when logged in (adapter lock
 * held). */
static void show_login_status(void)
{
    const char *msg = "";
    if (g_cfg->sp_client_id[0] == '\0') {
        msg = "Legg inn klient-ID fra Spotify p\xC3\xA5 oppsettsiden (Andre skjermer \xE2\x86\x92 Spotify).";
    } else {
        switch (spotify_login_state(NULL, 0)) {
        case SPOTIFY_NOT_CONNECTED:
            msg = "Koble skjermen til Spotify p\xC3\xA5 oppsettsiden (Andre skjermer \xE2\x86\x92 Spotify).";
            break;
        case SPOTIFY_CONNECTING:
            msg = "Kobler til Spotify...";
            break;
        case SPOTIFY_LOGIN_FAILED:
            msg = "Innloggingen hos Spotify mislyktes. Pr\xC3\xB8v igjen p\xC3\xA5 oppsettsiden.";
            break;
        case SPOTIFY_CONNECTED:
            msg = s_have ? "" : "Henter fra Spotify...";
            break;
        }
    }
    lv_label_set_text(g_status_label, msg);
    if (s_have && (g_cfg->sp_client_id[0] == '\0' || spotify_login_state(NULL, 0) != SPOTIFY_CONNECTED)) {
        s_have = false; /* nothing of a login that's gone */
        s_picker = false;
        spotify_show();
    }
}

/* Twice a second while on show: the clock, the progress bar, and a poll
 * when the track should have ended. */
static void pend_clear(void)
{
    s_pend.on = false;
    mpic_free(&s_pend.pic);
}

static void set_picker(bool on);

static void spotify_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    big_clock_set(&s_clock, time(NULL));
    if (s_pend.on && lv_tick_get() - s_pend.tick > SP_PEND_MS) {
        pend_clear(); /* Spotify never said: what it says now */
        spotify_show();
    }
    if (s_picker && lv_tick_get() - s_touch_tick > SP_PICKER_IDLE_MS && s_have && s_player.active) {
        set_picker(false); /* left open: back to what plays (and its cover) */
    }
    if (!showing_player() || s_pend.on) {
        return;
    }
    if (s_player.playing) {
        lv_area_t a;
        lv_obj_get_coords(s_canvas, &a);
        a.x1 += SP_COL_X - 10;
        a.x2 = a.x1 + SP_COL_W + 20;
        a.y1 += SP_PROG_Y - 8; /* the knob too */
        a.y2 = a.y1 + 18 + lv_font_get_line_height(g_font_body) + 4;
        lv_obj_invalidate_area(s_canvas, &a);
        if (!s_woke_at_end && s_player.duration_ms > 0 &&
            s_player.progress_ms + (int)(lv_tick_get() - s_player_tick) > s_player.duration_ms + 1000) {
            s_woke_at_end = true;
            app_wake_poll(); /* the next track */
        }
    }
    if (s_note[0] && lv_tick_get() - s_note_tick > SP_NOTE_MS) {
        lv_obj_invalidate(s_canvas);
    }
}

static void note(const char *msg)
{
    snprintf(s_note, sizeof(s_note), "%s", msg);
    s_note_tick = lv_tick_get();
}

/* --------------------------------------------------------------------------
 * Touch (LVGL context)
 * ------------------------------------------------------------------------ */

static void send(spotify_cmd_t cmd, int value, const char *device)
{
    s_cmd.pending = true;
    s_cmd.cmd = cmd;
    s_cmd.value = value;
    snprintf(s_cmd.device, sizeof(s_cmd.device), "%s", device ? device : "");
    s_cmd.uri[0] = '\0';
    lv_obj_invalidate(s_canvas);
    app_wake_poll();
}

/* Where a tile starts: the device playing (""), else the one that played
 * last if it is still listed (a phone switched off since isn't), else the
 * first that takes commands. */
static const char *play_device(void)
{
    if (s_player.active) {
        return "";
    }
    for (int i = 0; i < s_dev_count && s_last_device[0]; i++) {
        if (strcmp(s_devices[i].id, s_last_device) == 0 && !s_devices[i].restricted) {
            return s_last_device; /* if it is still there */
        }
    }
    for (int i = 0; i < s_dev_count; i++) {
        if (!s_devices[i].restricted) {
            return s_devices[i].id;
        }
    }
    return "";
}

static void set_picker(bool on)
{
    s_picker = on;
    note("");
    spotify_show();
    app_wake_poll(); /* the lists, or the cover */
}

static void set_page(page_t p)
{
    if (p != s_page && p != PG_QUEUE) {
        s_grid.count = 0; /* the other page's tiles not shown meanwhile: the poll fills them */
    }
    s_page = p;
    note("");
    lv_obj_invalidate(s_canvas);
    app_wake_poll();
}

/* "Starter <title> ..." at once, and where (lock held). */
static void pend_start(const char *title, const char *uri, const mgrid_tile_t *t)
{
    pend_clear();
    s_pend.on = true;
    snprintf(s_pend.title, sizeof(s_pend.title), "%s", title);
    snprintf(s_pend.uri, sizeof(s_pend.uri), "%s", uri);
    snprintf(s_pend.before_item, sizeof(s_pend.before_item), "%s", s_player.active ? s_player.item_id : "");
    s_pend.tick = lv_tick_get();
    const char *dev = s_player.active ? s_player.device.name : "";
    const char *id = play_device();
    for (int i = 0; i < s_dev_count && !dev[0] && id[0]; i++) {
        if (strcmp(s_devices[i].id, id) == 0) {
            dev = s_devices[i].name;
        }
    }
    snprintf(s_pend.where, sizeof(s_pend.where), "Starter%s%.60s \xE2\x80\xA6", dev[0] ? " p\xC3\xA5 " : "", dev);
    if (t) {
        mpic_take(&s_pend.pic, t);
    }
}

static bool near(int x, int y, int cx, int cy, int r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

bool spotify_tap(int x, int y)
{
    s_touch_tick = lv_tick_get();
    if (s_have) {
        lv_area_t a;
        pick_box(&a);
        if (x >= a.x1 - 6 && x <= a.x2 + 6 && y >= a.y1 - 8 && y <= a.y2 + 8) {
            set_picker(!s_picker);
            return true;
        }
    }
    if (s_picker) {
        const int tab = mtabs_hit(PAGE_NAMES, PG_COUNT, SP_X, SP_TAB_Y, SP_TAB_H, x, y);
        if (tab >= 0) {
            set_page((page_t)tab);
            return true;
        }
        if (s_page == PG_QUEUE) {
            const int i = mlist_hit(SP_GRID_TOP, s_queue_count, x, y);
            if (i < 0) {
                return false;
            }
            if (i > 0 || !s_player.active) {
                /* Spotify can't jump in the queue: skipped to it. */
                pend_start(s_queue[i].title, "", NULL);
                send(SPOTIFY_NEXT, s_player.active ? i : i + 1, NULL);
            }
            set_picker(false);
            return true;
        }
        const int i = mgrid_hit(&s_grid, x, y);
        if (i < 0) {
            return false;
        }
        pend_start(s_grid.t[i].title, s_grid.t[i].uri, &s_grid.t[i]);
        send(SPOTIFY_PLAY, 0, play_device());
        snprintf(s_cmd.uri, sizeof(s_cmd.uri), "%s", s_grid.t[i].uri);
        set_picker(false);
        return true;
    }
    if (s_pend.on) {
        return true; /* no controls meanwhile */
    }
    if (showing_player()) {
        const int cx = SP_COL_X + SP_COL_W / 2;
        if (device_locked()) {
            /* Greyed out: taken, so as not to change screen. */
            return x >= SP_COL_X && y >= SP_PROG_Y - 20;
        }
        if (near(x, y, cx, SP_CTRL_Y, SP_PLAY_R + 14)) {
            /* Shown at once; the answer after the command puts it right. */
            s_player.progress_ms = progress_now();
            s_player_tick = lv_tick_get();
            s_player.playing = !s_player.playing;
            send(s_player.playing ? SPOTIFY_PLAY : SPOTIFY_PAUSE, 0, NULL);
            return true;
        }
        if (near(x, y, cx - SP_SKIP_DX, SP_CTRL_Y, SP_SKIP_R + 16)) {
            send(SPOTIFY_PREVIOUS, 0, NULL);
            return true;
        }
        if (near(x, y, cx + SP_SKIP_DX, SP_CTRL_Y, SP_SKIP_R + 16)) {
            send(SPOTIFY_NEXT, 0, NULL);
            return true;
        }
        if (near(x, y, cx - SP_MODE_DX, SP_CTRL_Y, SP_MODE_R + 14)) {
            if (s_player.can_shuffle) {
                s_player.shuffle = !s_player.shuffle;
                send(SPOTIFY_SHUFFLE, s_player.shuffle, NULL);
            }
            return true;
        }
        if (near(x, y, cx + SP_MODE_DX, SP_CTRL_Y, SP_MODE_R + 14)) {
            if (s_player.can_repeat) {
                s_player.repeat = (s_player.repeat + 1) % 3; /* off, the list, the track */
                send(SPOTIFY_REPEAT, s_player.repeat, NULL);
            }
            return true;
        }
        const int x1 = SP_COL_X, x2 = SP_COL_X + SP_COL_W - 1;
        if (s_player.can_seek && y >= SP_PROG_Y - 18 && y <= SP_PROG_Y + 24 && x >= x1 - 10 && x <= x2 + 10) {
            const int px = x < x1 ? x1 : x > x2 ? x2 : x;
            s_player.progress_ms = (int)((int64_t)s_player.duration_ms * (px - x1) / (x2 - x1));
            s_player_tick = lv_tick_get();
            send(SPOTIFY_SEEK, s_player.progress_ms, NULL);
            return true;
        }
        if (s_player.device.has_volume && s_player.device.volume >= 0 && y > SP_VOL_Y - SP_VOL_R - 14 &&
            y < SP_VOL_Y + SP_VOL_R + 14 && x >= SP_COL_X) {
            /* - and + a step; the bar itself to where it's tapped. */
            const int mx = SP_COL_X + SP_VOL_R, plx = SP_COL_X + SP_COL_W - SP_VOL_R;
            const int bx1 = mx + SP_VOL_R + 16, bx2 = plx - SP_VOL_R - 16;
            int v = s_player.device.volume;
            if (x < bx1 - 8) {
                v = (v - 1) / SP_VOL_STEP * SP_VOL_STEP; /* the step below */
            } else if (x > bx2 + 8) {
                v = v / SP_VOL_STEP * SP_VOL_STEP + SP_VOL_STEP;
            } else {
                v = (x - bx1) * 100 / (bx2 - bx1);
            }
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            s_player.device.volume = v;
            send(SPOTIFY_VOLUME, v, NULL);
            return true;
        }
        /* Taps elsewhere in the right column do nothing, rather than
         * change screen by accident beside a button. */
        return x >= SP_COL_X && y >= SP_PROG_Y - 20;
    }
    if (s_have) {
        const int n = s_dev_count < 2 * dev_rows_max() ? s_dev_count : 2 * dev_rows_max();
        for (int i = 0; i < n; i++) {
            lv_area_t a;
            dev_box(i, &a);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                if (s_devices[i].restricted) {
                    note("Den enheten kan ikke styres herfra.");
                    lv_obj_invalidate(s_canvas);
                } else {
                    note("");
                    send(SPOTIFY_TRANSFER, 0, s_devices[i].id);
                }
                return true;
            }
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * The weather task
 * ------------------------------------------------------------------------ */

/* The cover's memory, while the screen is on show (see
 * spotify_release). */
/* The cover's memory given back: on another screen, or under the picker,
 * whose tiles need it. */
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

void spotify_release(void)
{
    drop_cover();
    mgrid_release(&s_grid);
}

/* The cover for `url`, if it isn't the one shown. */
static void fetch_cover(const char *url)
{
    /* A cover that failed is asked for again after a while (as on the
     * Sonos screen). */
    static int64_t tried_us;
    if (strcmp(url, s_cover_url) == 0 &&
        (s_cover_ok || url[0] == '\0' || esp_timer_get_time() - tried_us < SP_COVER_RETRY_MS * 1000LL)) {
        return;
    }
    uint16_t *px = NULL;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_cover_ok = false;
        px = s_cover_px;
        s_cover_px = NULL; /* the weather task's while it decodes */
        s_cover_img.data = NULL;
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
    if (url[0] && px == NULL) {
        px = heap_caps_aligned_alloc(16, (size_t)SP_COVER * SP_COVER * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (px == NULL) {
            ESP_LOGE(TAG, "No memory for the cover");
        }
    }
    bool ok = false;
    if (url[0] && px) {
        uint8_t *jpg = NULL;
        size_t len = 0;
        if (spotify_get_image(url, &jpg, &len) == ESP_OK) {
            ok = draw_picture_square(jpg, len, px, SP_COVER);
            free(jpg);
        }
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_cover_px = px;
        s_cover_img.data = (const uint8_t *)px;
        s_cover_ok = ok && px != NULL;
        snprintf(s_cover_url, sizeof(s_cover_url), "%s", url);
        tried_us = esp_timer_get_time();
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    } else {
        heap_caps_free(px);
    }
}

/* The page's message (`page`'s own, shown while it is). */
static void page_note(page_t page, const char *msg)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        snprintf(s_page_note[page], sizeof(s_page_note[page]), "%s", msg);
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
}

static void mark_playing(void);

/* The page's list asked for, if not lately, and its tiles made; the
 * lists' messages. */
static void fetch_page(page_t page)
{
    bool fetched = false;
    const char *msg = "";
    if (page == PG_RECENT && !stamp_fresh(&s_recent_at, SP_LISTS_MS)) {
        const esp_err_t err = spotify_get_recent(s_recent, MT_MAX, &s_recent_count);
        msg = err == SPOTIFY_ERR_SCOPE ? "Logg inn p\xC3\xA5 Spotify p\xC3\xA5 nytt (oppsettsiden) for \xC3\xA5 se alt her."
              : err != ESP_OK          ? "Fikk ikke hentet det som er spilt nylig. Pr\xC3\xB8ver igjen om litt."
              : s_recent_count == 0    ? "Ingenting spilt nylig."
                                       : "";
        /* A playlist played lately is named from the account's own, if known. */
        for (int i = 0; i < s_recent_count; i++) {
            for (int k = 0; k < s_lists_count && s_recent[i].name[0] == '\0'; k++) {
                if (strcmp(s_recent[i].uri, s_lists[k].uri) == 0) {
                    s_recent[i] = s_lists[k];
                }
            }
        }
        spotify_name_playlists(s_recent, s_recent_count, SP_NAMES_PER_POLL);
        stamp_now(&s_recent_at);
        fetched = true;
    } else if (page == PG_LISTS && !stamp_fresh(&s_lists_at, SP_LISTS_MS)) {
        const esp_err_t err = spotify_get_playlists(s_lists, MT_MAX, &s_lists_count);
        msg = err == SPOTIFY_ERR_SCOPE ? "Logg inn p\xC3\xA5 Spotify p\xC3\xA5 nytt (oppsettsiden) for \xC3\xA5 se alt her."
              : err != ESP_OK          ? "Fikk ikke hentet spillelistene. Pr\xC3\xB8ver igjen om litt."
              : s_lists_count == 0     ? "Ingen spillelister."
                                       : "";
        stamp_now(&s_lists_at);
        fetched = true;
    }
    if (fetched) {
        page_note(page, msg);
    } else if (s_built == (int)page) {
        return;
    }
    mgrid_tile_t *items = mgrid_items();
    if (items == NULL) {
        return; /* asked again next poll */
    }
    const spotify_recent_t *src = page == PG_RECENT ? s_recent : s_lists;
    const int count = page == PG_RECENT ? s_recent_count : s_lists_count;
    int n = 0;
    for (int i = 0; i < count && n < MT_MAX; i++, n++) {
        snprintf(items[n].uri, sizeof(items[n].uri), "%.63s", src[i].uri);
        snprintf(items[n].title, sizeof(items[n].title), "%s", src[i].name);
        snprintf(items[n].art, sizeof(items[n].art), "%s", src[i].image_url);
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        mgrid_set(&s_grid, items, n);
        s_built = page;
        mark_playing(); /* at once, not after the covers */
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
    free(items);
}

/* "Kø": what plays and what comes, asked again as the track changes. */
static void fetch_queue(const spotify_player_t *p)
{
    if (strcmp(p->item_id, s_queue_item) == 0 && stamp_fresh(&s_queue_at, SP_QUEUE_MS)) {
        return;
    }
    static EXT_RAM_BSS_ATTR spotify_track_t q[MT_LIST_MAX];
    int n = 0;
    const esp_err_t err = spotify_get_queue(q, MT_LIST_MAX, &n);
    snprintf(s_queue_item, sizeof(s_queue_item), "%s", p->item_id);
    stamp_now(&s_queue_at);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (err == ESP_OK) {
            memcpy(s_queue, q, sizeof(q[0]) * (size_t)n);
            s_queue_count = n;
        }
        snprintf(s_page_note[PG_QUEUE], sizeof(s_page_note[PG_QUEUE]), "%s",
                 err == SPOTIFY_ERR_SCOPE ? "Logg inn p\xC3\xA5 Spotify p\xC3\xA5 nytt (oppsettsiden) for \xC3\xA5 se k\xC3\xB8" "en."
                 : err != ESP_OK          ? "Fikk ikke hentet k\xC3\xB8" "en. Pr\xC3\xB8ver igjen om litt."
                 : n == 0                 ? "Ingenting spilles, og k\xC3\xB8" "en er tom."
                                          : "Trykk p\xC3\xA5 en sang for \xC3\xA5 hoppe dit.");
        lv_obj_invalidate(s_canvas);
        esp_lv_adapter_unlock();
    }
}

/* The tile of what plays ringed (adapter lock held). */
static void mark_playing(void)
{
    bool changed = false;
    for (int i = 0; i < s_grid.count; i++) {
        mgrid_tile_t *t = &s_grid.t[i];
        const bool on = s_player.active && s_player.context[0] && strcmp(t->uri, s_player.context) == 0;
        changed |= on != t->on;
        t->on = on;
    }
    if (changed && s_picker) {
        lv_obj_invalidate(s_canvas);
    }
}

/* A failed command or fetch, as the note under the cover. */
static const char *err_text(esp_err_t err)
{
    switch (err) {
    case SPOTIFY_ERR_PREMIUM:
        return "Spotify Premium trengs for \xC3\xA5 styre avspillingen.";
    case SPOTIFY_ERR_NO_DEVICE:
        return "Fant ingen enhet \xC3\xA5 spille p\xC3\xA5. \xC3\x85pne Spotify p\xC3\xA5 en av dem f\xC3\xB8rst.";
    case SPOTIFY_ERR_RATE:
        return "Spotify ber oss vente litt.";
    case SPOTIFY_ERR_RESTRICTED:
        return "Spotify lar ikke skjermen styre denne enheten.";
    default:
        return "Spotify svarte ikke. Pr\xC3\xB8v igjen.";
    }
}

uint32_t spotify_poll(int for_view)
{
    spotify_auth_service();
    if (lock_for_view(for_view)) {
        show_login_status();
        esp_lv_adapter_unlock();
    }
    if (g_cfg->sp_client_id[0] == '\0') {
        return SP_IDLE_MS;
    }
    switch (spotify_login_state(NULL, 0)) {
    case SPOTIFY_CONNECTED:
        break;
    case SPOTIFY_CONNECTING:
        return 2000;
    default:
        return SP_IDLE_MS;
    }

    /* A button pressed: send it first. */
    bool cmd = false, picker = false;
    page_t page = PG_RECENT;
    spotify_cmd_t c = SPOTIFY_PLAY;
    int value = 0;
    char device[48] = "", uri[64] = "";
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        cmd = s_cmd.pending;
        c = s_cmd.cmd;
        value = s_cmd.value;
        snprintf(device, sizeof(device), "%s", s_cmd.device);
        snprintf(uri, sizeof(uri), "%s", s_cmd.uri);
        s_cmd.pending = false;
        picker = s_picker;
        page = s_page;
        esp_lv_adapter_unlock();
    }
    if (!cmd && backoff_waiting(&s_fail, SP_RETRY_MS)) {
        return backoff_ms(&s_fail, SP_RETRY_MS);
    }
    if (cmd) {
        esp_err_t err;
        if (uri[0]) {
            err = spotify_play_context(uri, device);
        } else if (c == SPOTIFY_NEXT && value > 1) {
            /* To a track further on in "Kø": Spotify can't jump there. */
            err = ESP_OK;
            for (int k = 0; k < value && err == ESP_OK; k++) {
                err = spotify_command(SPOTIFY_NEXT, 0, NULL);
                vTaskDelay(pdMS_TO_TICKS(250));
            }
        } else {
            err = spotify_command(c, value, device);
        }
        if (err == SPOTIFY_ERR_RESTRICTED && c != SPOTIFY_TRANSFER && !uri[0]) {
            /* For good: the playing device's controls greyed out. */
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                snprintf(s_refused, sizeof(s_refused), "%s", s_player.device.id);
                esp_lv_adapter_unlock();
            }
        } else if (err != ESP_OK) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                note(err_text(err));
                pend_clear(); /* it didn't start: what plays, with why */
                esp_lv_adapter_unlock();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(c == SPOTIFY_TRANSFER || uri[0] ? 4 * SP_SETTLE_MS : SP_SETTLE_MS));
        }
    }

    esp_err_t err = spotify_get_player(&s_scratch);
    int dev_count = 0;
    if (err == ESP_OK && !s_scratch.active) {
        err = spotify_get_devices(s_dev_scratch, SPOTIFY_DEVICES_MAX, &dev_count);
    }
    if (err != ESP_OK) {
        diag_fail(DIAG_SPOTIFY, err);
        backoff_fail(&s_fail);
        if (lock_for_view(for_view)) {
            if (err == SPOTIFY_ERR_LOGIN) {
                show_login_status();
            } else if (s_have) {
                note(err_text(err));
                lv_obj_invalidate(s_canvas);
            } else {
                lv_label_set_text(g_status_label, "F\xC3\xA5r ikke kontakt med Spotify. Pr\xC3\xB8ver igjen...");
            }
            esp_lv_adapter_unlock();
        }
        if (err == SPOTIFY_ERR_RATE) {
            return (uint32_t)spotify_retry_after_s() * 1000;
        }
        return backoff_ms(&s_fail, SP_RETRY_MS);
    }
    diag_ok(DIAG_SPOTIFY);
    backoff_ok(&s_fail);
    bool more = false;
    if (picker) {
        drop_cover();
        if (page == PG_QUEUE) {
            mgrid_release(&s_grid);
            fetch_queue(&s_scratch);
        } else {
            fetch_page(page);
            more = mgrid_fetch(&s_grid, 3);
        }
    } else {
        mgrid_release(&s_grid); /* the tiles' memory for the cover: they come again with the picker */
        if (s_scratch.active) {
            fetch_cover(s_scratch.image_url);
        }
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_scratch.active && strcmp(s_scratch.device.id, s_player.device.id) != 0) {
            ESP_LOGI(TAG, "Playing on %s (%s)%s", s_scratch.device.name, s_scratch.device.type,
                     s_scratch.device.restricted ? ", restricted" : "");
        }
        if (strcmp(s_scratch.item_id, s_player.item_id) != 0 || !s_scratch.playing) {
            s_woke_at_end = false;
        }
        s_player = s_scratch;
        s_player_tick = lv_tick_get();
        if (s_player.active && s_player.device.id[0]) {
            snprintf(s_last_device, sizeof(s_last_device), "%s", s_player.device.id);
        }
        memcpy(s_devices, s_dev_scratch, sizeof(s_devices));
        s_dev_count = dev_count;
        s_have = true;
        /* "Starter ...": till another track plays, or the context tapped. */
        if (s_pend.on && s_player.active &&
            (strcmp(s_player.item_id, s_pend.before_item) != 0 ||
             (s_pend.uri[0] && strcmp(s_player.context, s_pend.uri) == 0))) {
            pend_clear();
        }
        mark_playing();
        esp_lv_adapter_unlock();
    }
    if (lock_for_view(for_view)) {
        show_login_status();
        spotify_show();
        esp_lv_adapter_unlock();
    }
    if (more) {
        return 100; /* the rest of the covers */
    }
    if (s_pend.on) {
        return SP_PEND_POLL_MS; /* what was tapped, as soon as it plays */
    }
    return s_scratch.active && s_scratch.playing ? SP_PLAYING_MS : SP_PAUSED_MS;
}

/* --------------------------------------------------------------------------
 * Building
 * ------------------------------------------------------------------------ */

static lv_obj_t *text_label(const lv_font_t *font, lv_color_t colour, int y)
{
    lv_obj_t *l = lv_label_create(s_root);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_pos(l, SP_COL_X, y);
    lv_obj_set_width(l, SP_COL_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font));
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

lv_obj_t *spotify_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    if (g_cfg->sp_dark && g_cfg->theme != APP_THEME_DARK) {
        /* Dark on its own: the dark theme's background and text. */
        lv_obj_set_style_bg_color(s_root, lv_color_hex(0x15171A), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_root, lv_palette_lighten(LV_PALETTE_GREY, 5), 0);
    }
    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, spotify_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, SP_X, 4);
    lv_label_set_text(s_title, "Spotify");
    big_clock_create(&s_clock, s_root, SP_X);

    s_track = text_label(g_font_large, c_txt(), SP_TOP);
    s_artist = text_label(g_font_body, c_txt(), SP_TOP + lv_font_get_line_height(g_font_large) + 8);
    s_album = text_label(g_font_body, c_dim(), SP_TOP + 2 * lv_font_get_line_height(g_font_large));
    s_msg = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_msg, g_font_large, 0);
    lv_obj_set_style_text_color(s_msg, c_txt(), 0);
    lv_obj_set_pos(s_msg, SP_X, SP_TOP + 8);
    lv_obj_set_width(s_msg, BOARD_LCD_H_RES - 2 * SP_X);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);

    mgrid_init(&s_grid, s_canvas, SP_GRID_TOP);
    /* The pixels come with the first cover (fetch_cover). */
    s_cover_img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = SP_COVER, .h = SP_COVER,
                    .stride = SP_COVER * 2 },
        .data_size = SP_COVER * SP_COVER * 2,
    };
    lv_timer_create(spotify_tick_cb, SP_TICK_MS, NULL);
    return s_root;
}

void spotify_enter(void)
{
    big_clock_set(&s_clock, time(NULL));
    note("");
    s_picker = false; /* the player first */
    s_touch_tick = lv_tick_get();
    show_login_status();
    spotify_show();
}

/* --------------------------------------------------------------------------
 * Logging in (the web server's task)
 * ------------------------------------------------------------------------ */

/* GET /spotify/login (password as for the setup page): on to Spotify. */
static esp_err_t h_login(httpd_req_t *req)
{
    char host[96] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *url = malloc(1024);
    if (url == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    if (!spotify_auth_url(host, url, 1024)) {
        free(url);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Lagre en klient-ID fra Spotify p\xC3\xA5 oppsettsiden f\xC3\xB8rst. Innloggingen "
                                       "trenger ogs\xC3\xA5 at oppdateringsadressen begynner med https://.");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, NULL, 0);
    free(url);
    return err;
}

/* GET /spotify/svar?code=..&state=.. (or &error=..), from the update
 * site's page: no password, as the login cookie doesn't come along from
 * another site; the state, made for this login only, is the check. */
static esp_err_t h_return(httpd_req_t *req)
{
    const size_t qlen = httpd_req_get_url_query_len(req) + 1;
    char *q = qlen > 1 && qlen < 2048 ? malloc(qlen) : NULL;
    char *code = malloc(1536); /* longer than any code kept (SP_CODE_MAX), so a cut one is refused */
    char state[160] = "", error[64] = "";
    bool ok = false;
    if (q != NULL && code != NULL && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
        code[0] = '\0';
        form_field(q, "code", code, 1536);
        form_field(q, "state", state, sizeof(state));
        form_field(q, "error", error, sizeof(error));
        ok = spotify_auth_return(state, code, error);
    }
    free(q);
    free(code);
    if (ok) {
        app_wake_poll(); /* the weather task trades the code */
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* On to the setup page from here, a page of the display's own, so the
     * login cookie comes along. */
    return httpd_resp_sendstr(
        req, ok ? "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                  "<meta http-equiv=refresh content='4;url=/oppsett#spotify'><title>Spotify</title>"
                  "<body style='font-family:sans-serif;margin:2em'><p>Kobler skjermen til Spotify...</p>"
                  "<p><a href='/oppsett#spotify'>Tilbake til oppsettet</a></p>"
                : "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                  "<title>Spotify</title><body style='font-family:sans-serif;margin:2em'>"
                  "<p>Innloggingen hos Spotify ble avbrutt eller er for gammel. Pr\xC3\xB8v igjen fra "
                  "oppsettsiden.</p><p><a href='/oppsett#spotify'>Tilbake til oppsettet</a></p>");
}

/* POST /spotify/koble-fra (password, same origin): forget the login. */
static esp_err_t h_logout(httpd_req_t *req)
{
    spotify_disconnect();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/oppsett#spotify");
    return httpd_resp_send(req, NULL, 0);
}

void spotify_add_handlers(void)
{
    wifi_provision_add_get_handler("/spotify/login", h_login);
    wifi_provision_add_open_get_handler("/spotify/svar", h_return);
    wifi_provision_add_post_handler("/spotify/koble-fra", h_logout);
}
