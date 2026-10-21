#ifndef _WATCHDOG_H_
#define _WATCHDOG_H_

/* Restart if the LVGL task stalls for a minute, the weather task for 20
 * minutes, or memory stays low for a minute; log the memory figures hourly
 * (see watchdog.c). Call once the UI is built. */
void wd_start(void);

/* The weather task's sign of life; it's watched from its first beat. */
void wd_weather_beat(void);

/* PSRAM a screen holds on purpose while on show and gives back when it
 * leaves (the music pickers' tiles), `delta` bytes more or fewer: counted
 * as free for the low-memory rule, as nothing else needs it meanwhile. Any
 * task. */
void wd_psram_held(int delta);

/* If the watchdog caused this boot: why, in Norwegian ("skjermen hadde
 * stanset", ...); else NULL. */
const char *wd_boot_trip(void);

#endif
