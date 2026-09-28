/* Included by LVGL's lv_assert.h (CONFIG_LV_ASSERT_HANDLER_INCLUDE). LVGL's
 * default assert handler is `while(1);`: a failed allocation inside LVGL
 * would spin its task forever with the display lock held, freezing the
 * screen and the web server with no way back. Abort instead - the panic
 * handler logs a backtrace and restarts the device. */
#pragma once
#include <assert.h>
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER __builtin_abort();
