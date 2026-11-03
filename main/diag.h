#pragma once

/* Diagnostics over WiFi (see diag.c): the recent log (/log), a crash dump
 * (/coredump), and the display's health with each service's last result
 * (/status). */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* The services whose last fetch /status reports. */
typedef enum {
    DIAG_FORECAST, DIAG_NOWCAST, DIAG_ALERTS, DIAG_AIRCRAFT, DIAG_SHIPS, DIAG_RAIN, DIAG_DEPARTURES,
    DIAG_ROUTES, DIAG_OWNERS, DIAG_AIR, DIAG_POLLEN, DIAG_TIDE, DIAG_CALENDAR, DIAG_SATELLITE, DIAG_AURORA,
    DIAG_SPOTIFY, DIAG_SONOS, DIAG_ISS, DIAG_SERVICE_COUNT
} diag_service_t;

/* Start keeping the log. Call first thing in app_main. */
void diag_init(void);

/* Serve /log, /coredump and /status; once connected. */
void diag_start(void);

/* A fetch by `svc` succeeded / failed with `err`. */
void diag_ok(diag_service_t svc);
void diag_fail(diag_service_t svc, esp_err_t err);

/* Whether any fetch has worked since boot. */
bool diag_any_ok(void);

/* What GET /status answers, as JSON text (cJSON_free it); NULL without
 * memory. */
char *diag_status_json(void);

/* What GET /log answers: the log kept since start, oldest first, as text
 * of *len bytes and a '\0' in PSRAM (heap_caps_free it); NULL if there is
 * none or no memory. */
char *diag_log_text(size_t *len);

typedef enum { DIAG_ONLINE, DIAG_NO_WIFI, DIAG_NO_INTERNET } diag_net_t;

/* Whether the display is online: WiFi down, or WiFi up but every fetch
 * failing for a while. */
diag_net_t diag_net_state(void);
