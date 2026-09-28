#ifndef _WATCHDOG_H_
#define _WATCHDOG_H_

/* Restart if the LVGL task stalls for a minute or the weather task for 20
 * minutes (see watchdog.c). Call once the UI is built. */
void wd_start(void);

/* The weather task's sign of life; it's watched from its first beat. */
void wd_weather_beat(void);

#endif
