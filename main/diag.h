#pragma once

/* Diagnostics over WiFi (see diag.c): the recent log (/log), a crash dump
 * (/coredump), and the display's health with each service's last result
 * (/status). */

#include <stdbool.h>

#include "esp_err.h"

/* The services whose last fetch /status reports. */
typedef enum {
    DIAG_FORECAST, DIAG_NOWCAST, DIAG_ALERTS, DIAG_AIRCRAFT, DIAG_SHIPS, DIAG_RAIN, DIAG_DEPARTURES,
    DIAG_ROUTES, DIAG_AIR, DIAG_POLLEN, DIAG_TIDE, DIAG_CALENDAR, DIAG_SATELLITE, DIAG_SERVICE_COUNT
} diag_service_t;

/* Start keeping the log. Call first thing in app_main. */
void diag_init(void);

/* Serve /log, /coredump and /status; once connected. */
void diag_start(void);

/* A fetch by `svc` succeeded / failed with `err`. */
void diag_ok(diag_service_t svc);
void diag_fail(diag_service_t svc, esp_err_t err);

typedef enum { DIAG_ONLINE, DIAG_NO_WIFI, DIAG_NO_INTERNET } diag_net_t;

/* Whether the display is online: WiFi down, or WiFi up but every fetch
 * failing for a while. */
diag_net_t diag_net_state(void);
