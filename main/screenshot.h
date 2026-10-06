#ifndef _SCREENSHOT_H_
#define _SCREENSHOT_H_

#include "esp_http_server.h"
#include "esp_lcd_panel_ops.h"

/* Watch which of the RGB panel's `fb_count` frame buffers is on show, so a
 * screenshot can be read straight from it. Before the display is used. */
void screenshot_init(esp_lcd_panel_handle_t panel, int fb_count);

/* GET /screen.png: the screen as it is now, as a PNG. */
esp_err_t screenshot_handler(httpd_req_t *req);

#endif
