/* LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC), in PSRAM.
 *
 * With the C library's malloc, everything LVGL allocated under
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (1 KB) - every widget, style, label
 * text and FreeType glyph-cache entry (FreeType allocates through LVGL,
 * LV_FREETYPE_USE_LVGL_PORT) - went to internal DRAM, which WiFi and TLS
 * need. None of it is used for DMA (the draw and bounce buffers are
 * allocated by the adapter and the LCD driver), so it all goes to PSRAM,
 * falling back to internal DRAM only if PSRAM is full. */

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_MEM_CAPS_FIRST (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CAPS_THEN (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, LV_MEM_CAPS_FIRST, LV_MEM_CAPS_THEN);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, LV_MEM_CAPS_FIRST, LV_MEM_CAPS_THEN);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
