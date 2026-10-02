#ifndef _CAL_CLIENT_H_
#define _CAL_CLIENT_H_

/* The calendar screen's calendars: each iCal address fetched and read as it
 * streams in (ical.h), merged into one list. */

#include <stdint.h>

#include "esp_err.h"
#include "ical.h"

/* Fetch the `n` addresses in urls[] ("" = unused) into `out`, keeping the
 * events that overlap [from, to); calendar i's events have feed i. ESP_OK
 * if every calendar was read; otherwise *failed has bit i set for each one
 * that wasn't (its events are left out), and the result is ESP_FAIL only
 * if none was. */
esp_err_t cal_fetch(const char *const *urls, int n, int64_t from, int64_t to, cal_list_t *out, uint8_t *failed);

#endif
