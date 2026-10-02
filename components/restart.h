#ifndef _RESTART_H_
#define _RESTART_H_

/* esp_restart(), always from core 0 - see restart.c. Doesn't return. */
void restart_device(void) __attribute__((noreturn));

#endif
