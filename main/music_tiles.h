#ifndef _MUSIC_TILES_H_
#define _MUSIC_TILES_H_

/* The grid of "Velg musikk" on the Sonos and Spotify screens: two rows of
 * square tiles, each a cover (or a word drawn on it, for a radio channel)
 * with its name under it. A screen fills the tiles under the adapter lock
 * (mgrid_set), the weather task fetches the covers (mgrid_fetch) into pixels held
 * only while the grid can be shown (mgrid_release). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_lv_adapter.h"

#define MT_TILE    104 /* px square (a multiple of 8, for the decoder) */
#define MT_COLS    6
#define MT_MAX     (2 * MT_COLS)
#define MT_URL_MAX 320

typedef struct {
    int idx;                 /* the screen's own: which item */
    char uri[64];            /* the screen's own: what to play */
    char title[64];          /* under the tile */
    char label[24];          /* drawn on the tile while it has no picture, "" = none */
    bool dot;                /* Spotify's green in a corner */
    char art[MT_URL_MAX];    /* the cover's address, "" = none; "SHEET#KEY" = logo
                              * KEY in the sheet at SHEET (sonos_logo_find),
                              * fetched once for all its tiles */
    /* The weather task's to allocate and fill, `ok` false meanwhile. */
    uint16_t *px;            /* MT_TILE square, RGB565, PSRAM; NULL = none yet */
    uint32_t px_art;         /* mgrid_art_hash of the picture in px (or tried
                              * for it), 0 = none */
    bool ok;
    lv_image_dsc_t img;
    bool on;                 /* the screen's: what plays now, ringed; false after mgrid_set */
} mgrid_tile_t;

typedef struct {
    mgrid_tile_t t[MT_MAX];
    int count;
    int top;          /* the first row's y */
    lv_obj_t *canvas; /* drawn on, invalidated as covers come */
} mgrid_t;

void mgrid_init(mgrid_t *g, lv_obj_t *canvas, int top);

/* Tile i's box, with its name. */
void mgrid_box(const mgrid_t *g, int i, lv_area_t *a);

/* The tile at x, y; -1 if none. */
int mgrid_hit(const mgrid_t *g, int x, int y);

/* The grid; a tile without a cover in `card` (a label's in `label_bg`),
 * the one playing ringed in `mark`. */
void mgrid_draw(lv_layer_t *layer, const mgrid_t *g, lv_color_t card, lv_color_t bg, lv_color_t txt,
                lv_color_t label_bg, lv_color_t mark);

/* A `side` px square in `bg` at x, y with `txt` on it in white, wrapped
 * and centred, in the biggest font each of its words fits in: a radio
 * channel's tile. */
void mgrid_draw_word(lv_layer_t *layer, const char *txt, int x, int y, int side, lv_color_t bg);

/* `art`'s hash: what tells one picture from another; 0 for "". */
uint32_t mgrid_art_hash(const char *art);

/* Room for the tiles mgrid_set takes, zeroed (PSRAM, only meanwhile:
 * free() it); NULL if there is none. */
mgrid_tile_t *mgrid_items(void);

/* Adapter lock held: the tiles are now items[0..n) (only idx, uri, title,
 * label, dot and art are taken). Covers already fetched stay with their
 * picture. */
void mgrid_set(mgrid_t *g, const mgrid_tile_t *items, int n);

/* Weather task: up to `max` covers still missing fetched; true if more
 * remain. */
bool mgrid_fetch(mgrid_t *g, int max);

/* Weather task: the covers' memory given back. */
void mgrid_release(mgrid_t *g);

/* A tile's picture (or its label) kept for a while after the grid has
 * gone: the player's "Starter ..." while what was tapped starts. */
typedef struct {
    uint16_t *px; /* MT_TILE square, PSRAM; NULL = none */
    lv_image_dsc_t img;
    char label[24];
} mpic_t;

/* Tile t's picture copied into p (adapter lock held); false without one
 * (its label is taken then, if any). */
bool mpic_take(mpic_t *p, const mgrid_tile_t *t);
void mpic_free(mpic_t *p);

/* p `side` px square at x, y: the picture scaled up, else its label on
 * `label_bg`, else a plain `card`. */
void mpic_draw(lv_layer_t *layer, const mpic_t *p, int x, int y, int side, lv_color_t card, lv_color_t label_bg);

/* A row of tabs ("Favoritter", "Radio", "Kø", ...) from x at y, `h` high. */
void mtabs_box(const char *const *labels, int i, int x, int y, int h, lv_area_t *a);
int mtabs_hit(const char *const *labels, int n, int x, int y, int h, int px, int py); /* -1 = none */
void mtabs_draw(lv_layer_t *layer, const char *const *labels, int n, int sel, int x, int y, int h, lv_color_t fill,
                lv_color_t sel_fill, lv_color_t txt, lv_color_t sel_txt);

/* "Kø": a list of tracks in two columns from `top` down to the note line
 * at the bottom, item i's box; MT_LIST_MAX of them. */
#define MT_LIST_ROWS 4
#define MT_LIST_MAX  (2 * MT_LIST_ROWS)
void mlist_box(int top, int i, lv_area_t *a);
int mlist_hit(int top, int n, int x, int y); /* -1 = none */
void mlist_draw_item(lv_layer_t *layer, int top, int i, const char *title, const char *artist, bool current,
                     lv_color_t card, lv_color_t txt, lv_color_t dim, lv_color_t cur_fill, lv_color_t cur_txt);

#endif
