#ifndef _YR_CLIENT_H_
#define _YR_CLIENT_H_

#include <stdbool.h>
#include "esp_err.h"

#define YR_FORECAST_MAX_POINTS 48

typedef struct {
    char hour_minute[6];        /* "HH:MM", local API time (UTC), for axis labels */
    bool is_first_of_day;       /* true if this point is the first hour (00:xx) of a new day */
    float air_temperature_c;
    float wind_speed_ms;
    float precipitation_mm;     /* best-effort 1h precipitation (see yr_client.c) */
    char symbol_code[48];       /* e.g. "partlycloudy_day" */
} yr_forecast_point_t;

typedef struct {
    bool valid;
    char updated_time[32];      /* ISO8601 timestamp of forecast issue time */
    int point_count;            /* number of valid entries in points[] (<= YR_FORECAST_MAX_POINTS) */
    yr_forecast_point_t points[YR_FORECAST_MAX_POINTS];
} yr_forecast_t;

/**
 * Fetch up to the next YR_FORECAST_MAX_POINTS hourly forecast points for
 * (lat, lon) from the MET Norway (yr.no) Locationforecast API.
 *
 * lat/lon are formatted with at most 4 decimals as required by the API.
 */
esp_err_t yr_client_fetch_forecast(double lat, double lon, yr_forecast_t *out);

#endif
