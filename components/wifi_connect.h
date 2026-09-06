#ifndef _WIFI_CONNECT_H_
#define _WIFI_CONNECT_H_

#include "esp_err.h"

/**
 * Initialize NVS, netif and the WiFi driver, then connect in station mode
 * using CONFIG_EXAMPLE_WIFI_SSID / CONFIG_EXAMPLE_WIFI_PASSWORD and block
 * until an IP address has been obtained (or the connection definitively
 * fails after internal retries).
 */
esp_err_t wifi_connect_sta(void);

#endif
