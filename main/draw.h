#ifndef _DRAW_H_
#define _DRAW_H_

#include "esp_lv_adapter.h"

/* Whether an area (or x1..x2, y1..y2) meets the strip being drawn. */
bool draw_area_visible(const lv_layer_t *layer, const lv_area_t *a);
bool draw_visible(const lv_layer_t *layer, int x1, int y1, int x2, int y2);

/* One line of body-font text in a box `w` wide at x, y. */
void draw_text(lv_layer_t *layer, const char *txt, int x, int y, int w, lv_text_align_t align, lv_color_t color);
/* The same, left-aligned and cut short with ".." to fit `w`. */
void draw_text_fit(lv_layer_t *layer, const char *txt, int x, int y, int w, lv_color_t color);
/* `txt` in `font` into buf, cut short with ".." (never inside a UTF-8
 * character) to fit `w` px on one line. buf_len at least 4. */
void draw_text_shorten(const lv_font_t *font, const char *txt, int w, char *buf, size_t buf_len);
/* Lines `txt` takes wrapped to `w` px (1..max_lines). */
int draw_text_lines(const char *txt, int w, int max_lines);
/* `txt` wrapped to `w` px over at most `lines` lines. */
void draw_text_wrap(lv_layer_t *layer, const char *txt, int x, int y, int w, int lines, lv_color_t color);
/* Width of `txt` in the body font. */
int draw_text_w(const char *txt);

void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, lv_color_t color);
void draw_triangle(lv_layer_t *layer, const int pts[3][2], lv_color_t color);
/* A filled rectangle x1..x2, y1..y2 (inclusive), corners rounded by radius. */
void draw_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, int radius, lv_color_t color);
/* A filled circle of radius r around cx, cy. */
void draw_dot(lv_layer_t *layer, int cx, int cy, int r, lv_color_t color);

/* A JPEG's or PNG's middle square at `side` x `side` px, as RGB565 into
 * out (16-byte aligned, side * side pixels). A JPEG is scaled down while
 * decoding where the decoder can (side a multiple of 8), and a PNG decoded
 * a row at a time, so a big picture needs no buffer of its full size; a
 * PNG's transparency is drawn over white. False if it can't be read. */
bool draw_picture_square(const uint8_t *data, size_t len, uint16_t *out, int side);

#endif
