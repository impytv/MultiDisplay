/* The wall clock. Night mode, the nightly restart and the update window all
 * need it, so it doesn't rest on one server:
 *
 *  - SNTP with up to three servers: the router's, if its DHCP answer names
 *    one (CONFIG_LWIP_DHCP_GET_NTP_SRV), then pool.ntp.org and
 *    time.cloudflare.com;
 *  - until NTP answers, the Date header of the first HTTPS response from
 *    MET (http_util.c) sets it to within a second or so.
 *
 * The screen says "Klokken er ikke stilt" if neither has worked after ten
 * minutes (main.c). */

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "app.h"
#include "clock.h"
#include "http_util.h"

static const char *TAG = "clock";

static volatile time_t s_ntp_at;

static void on_sync(struct timeval *tv)
{
    s_ntp_at = tv->tv_sec;
    ESP_LOGI(TAG, "Clock set by NTP");
}

void clock_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sync;
    /* The router's server (from DHCP) first; the two above after it. */
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.index_of_first_server = 1;
    esp_netif_sntp_init(&cfg);
}

bool clock_is_set(void)
{
    return time(NULL) > PLAUSIBLE_EPOCH_S;
}

time_t clock_set_at(const char **source)
{
    if (s_ntp_at != 0) {
        *source = "NTP";
        return s_ntp_at;
    }
    const time_t http = http_clock_set_at();
    *source = http != 0 ? "HTTP" : "";
    return http;
}
