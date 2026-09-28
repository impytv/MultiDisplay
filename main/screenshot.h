#ifndef _SCREENSHOT_H_
#define _SCREENSHOT_H_

#include "esp_http_server.h"

/* GET /screen.png: the screen as it is now, as a PNG. */
esp_err_t screenshot_handler(httpd_req_t *req);

#endif
