#ifndef _YR_CLIENT_H_
#define _YR_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Hourly points taken from the Locationforecast API. */
#define YR_FORECAST_BASE_POINTS 48
/* Array capacity: base hourly points plus the finer Nowcast points that get
 * spliced in ahead of them for the next ~2 hours (see main.c merge_nowcast). */
#define YR_FORECAST_MAX_POINTS 72
/* 5-minute Nowcast steps (~2 hours of radar precipitation nowcast). */
#define YR_NOWCAST_MAX_POINTS 30

typedef struct {
    char hour_minute[6];        /* "HH:MM", Europe/Oslo local time, for axis labels */
    bool is_first_of_day;       /* true if this point is the first hour (00:xx) of a new local day */
    int64_t epoch_utc;          /* seconds since the Unix epoch, for time-delta arithmetic */
    float air_temperature_c;
    float wind_speed_ms;
    float wind_from_deg;        /* direction the wind blows FROM, degrees clockwise from north */
    float precipitation_mm;     /* best-effort 1h precipitation (see yr_client.c) */
    char symbol_code[48];       /* e.g. "partlycloudy_day" */
} yr_forecast_point_t;

typedef struct {
    bool valid;
    char updated_time[32];         /* ISO8601 UTC timestamp of forecast issue time */
    char updated_hour_minute[6];   /* "HH:MM", Europe/Oslo local time, of the issue time */
    int point_count;               /* number of valid entries in points[] (<= YR_FORECAST_MAX_POINTS) */
    yr_forecast_point_t points[YR_FORECAST_MAX_POINTS];
} yr_forecast_t;

typedef struct {
    char hour_minute[6];        /* "HH:MM", Europe/Oslo local time */
    bool is_first_of_day;
    int64_t epoch_utc;
    float precipitation_rate;   /* mm/h, radar nowcast (comparable unit to the hourly amounts) */
    float air_temperature_c;    /* only meaningful when has_instant_details (first step only) */
    float wind_speed_ms;        /* only meaningful when has_instant_details */
    float wind_from_deg;        /* only meaningful when has_instant_details */
    bool has_instant_details;   /* true for the steps that carry temperature/wind (the first) */
    char symbol_code[48];       /* usually only the first step carries one; "" otherwise */
} yr_nowcast_point_t;

typedef struct {
    bool valid;
    bool radar_ok;                 /* properties.meta.radar_coverage == "ok" */
    char updated_hour_minute[6];   /* "HH:MM", Europe/Oslo local time, of the nowcast issue time */
    int point_count;
    yr_nowcast_point_t points[YR_NOWCAST_MAX_POINTS];
} yr_nowcast_t;

/**
 * Fetch up to YR_FORECAST_BASE_POINTS hourly forecast points for (lat, lon)
 * from the MET Norway (yr.no) Locationforecast API.
 *
 * lat/lon are formatted with at most 4 decimals as required by the API.
 */
esp_err_t yr_client_fetch_forecast(double lat, double lon, yr_forecast_t *out);

/**
 * Fetch the MET Norway Nowcast (5-minute radar precipitation nowcast for the
 * next ~2 hours). Nordic coverage only; check out->radar_ok before trusting
 * the precipitation values.
 */
esp_err_t yr_client_fetch_nowcast(double lat, double lon, yr_nowcast_t *out);

#endif
