#ifndef _WATCHDOG_H_
#define _WATCHDOG_H_

/* Restart if the LVGL task stalls for a minute, the weather task for 20
 * minutes, or memory stays low for a minute; log the memory figures hourly
 * (see watchdog.c). Call once the UI is built. */
void wd_start(void);

/* The weather task's sign of life; it's watched from its first beat. */
void wd_weather_beat(void);

#endif
