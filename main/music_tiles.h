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

/* The grid; a tile without a cover in `card` (a label's in `label_bg`). */
void mgrid_draw(lv_layer_t *layer, const mgrid_t *g, lv_color_t card, lv_color_t bg, lv_color_t txt,
             lv_color_t label_bg);

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

#endif
