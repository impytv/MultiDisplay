#ifndef _WIFI_PROVISION_H_
#define _WIFI_PROVISION_H_

#include "esp_err.h"
#include "esp_http_server.h"
#include "app_config.h"

/* Called with short human-readable status lines so the caller can show them
 * on the display (e.g. "Kobler til <ssid>...", or the portal instructions).
 * May be NULL. Invoked from the provisioning task. */
typedef void (*wifi_provision_status_fn)(const char *msg);

/**
 * Bring up networking:
 *
 *  - If the device has never been set up, or the BOOT button (GPIO0) is held
 *    at boot, or the stored WiFi credentials fail to connect, start the
 *    captive setup portal: a "MultiDisplay-XXXX" access point serving a
 *    config web page at http://192.168.4.1/. Saving the form writes NVS and
 *    reboots - so in that case this function never returns.
 *
 *  - Otherwise connect in station mode using `cfg` and return ESP_OK once an
 *    IP is obtained. The same config web page is then also served on the
 *    station IP for later changes.
 *
 * NVS must already be initialised. `cfg` is copied.
 */
esp_err_t wifi_provision_connect(const app_config_t *cfg, wifi_provision_status_fn status);

/**
 * The station's current IP address as a string (e.g. "192.168.0.77"), or ""
 * if it has never associated. Safe to call from any task - set once from the
 * WiFi event handler on IP_EVENT_STA_GOT_IP and never freed.
 */
const char *wifi_provision_get_ip(void);

/**
 * Serve GET `uri` with `handler` on the config web server (which is running
 * once wifi_provision_connect has returned), ahead of its catch-all page.
 */
esp_err_t wifi_provision_add_get_handler(const char *uri, esp_err_t (*handler)(httpd_req_t *req));

#endif
