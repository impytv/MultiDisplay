/* The microSD (TF) card slot of the Waveshare ESP32-S3-Touch-LCD-4.3B (and
 * -7): SPI on GPIO 11 (MOSI), 12 (SCK) and 13 (MISO), with the card's chip
 * select on the CH422G expander's EXIO4. An expander pin can't follow each
 * SPI transfer, so it is held low (selected) for good - the card is all
 * there is on this bus - as Waveshare's own examples do (see
 * waveshare_rgb_lcd_port.c), and the SPI driver is told there is no CS.
 *
 * exFAT (as cards over 32 GB come), FAT32 or FAT16: components/fatfs builds
 * ESP-IDF's FatFs with exFAT on, and long file names in UTF-8
 * (sdkconfig.defaults). Not NTFS.
 *
 * Mounted when first asked for, not at start, so a display without a card
 * spends nothing on it; tried again at most every SD_RETRY_MS while there
 * is none, as a try takes a moment. */

#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define SD_HOST      SPI2_HOST
#define SD_PIN_MOSI  11
#define SD_PIN_SCK   12
#define SD_PIN_MISO  13
#define SD_RETRY_MS  10000

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_bus;          /* the SPI bus is set up (once) */
static sdmmc_card_t *s_card; /* mounted */
static int64_t s_tried_us;  /* the last failed try */
static bool s_tried;
static const char *s_unreadable; /* what a card that answered holds instead */
static char s_drive[8];          /* FatFs's drive for it, "0:", while mounted */

/* What is on a card that answered but has no file system FatFs can mount,
 * from its first sectors: logged, and told the page, so "no card" isn't
 * all anyone sees. Same bus and slot as the mount, opened for this alone. */
static bool sig_ok(const uint8_t *s)
{
    return s[510] == 0x55 && s[511] == 0xAA;
}

static const char *what_is_on(const sdmmc_host_t *host_cfg, const sdspi_device_config_t *slot)
{
    const char *what = "ukjent";
    uint8_t *s = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    sdspi_dev_handle_t dev;
    if (s == NULL || sdspi_host_init() != ESP_OK) {
        heap_caps_free(s);
        return what;
    }
    if (sdspi_host_init_device(slot, &dev) == ESP_OK) {
        sdmmc_host_t host = *host_cfg;
        host.slot = dev;
        sdmmc_card_t card;
        if (sdmmc_card_init(&host, &card) == ESP_OK && sdmmc_read_sectors(&card, s, 0, 1) == ESP_OK) {
            ESP_LOGW(TAG, "Card %s, %llu MB", card.cid.name,
                     (unsigned long long)card.csd.capacity * card.csd.sector_size / (1024 * 1024));
            /* A boot sector names its file system at bytes 3..10; sector 0
             * is one (a card without partitions) or holds the MBR's table,
             * whose first entry's type is at 450 and start at 454. */
            bool boot = sig_ok(s) && (memcmp(s + 3, "NTFS", 4) == 0 || memcmp(s + 3, "EXFAT", 5) == 0 ||
                                      memcmp(s + 82, "FAT32", 5) == 0 || memcmp(s + 54, "FAT", 3) == 0);
            if (!sig_ok(s)) {
                ESP_LOGW(TAG, "Sector 0 isn't signed: not formatted");
                what = "uformatert";
            } else if (!boot) {
                const uint8_t type = s[450];
                const uint32_t first = (uint32_t)s[454] | (uint32_t)s[455] << 8 | (uint32_t)s[456] << 16 |
                                       (uint32_t)s[457] << 24;
                ESP_LOGW(TAG, "Partition 1: type 0x%02X at sector %lu", type, (unsigned long)first);
                if (type == 0) {
                    what = "uformatert";
                } else if (type == 0xEE) {
                    what = "gpt"; /* read, so its partition is something else */
                } else if (sdmmc_read_sectors(&card, s, first, 1) == ESP_OK) {
                    boot = true;
                }
            }
            if (boot) {
                char name[9];
                for (int i = 0; i < 8; i++) {
                    name[i] = s[3 + i] >= 0x20 && s[3 + i] < 0x7F ? (char)s[3 + i] : '?';
                }
                name[8] = '\0';
                ESP_LOGW(TAG, "Boot sector: '%s'%s", name, sig_ok(s) ? "" : ", unsigned");
                what = !sig_ok(s) ? "uformatert" : memcmp(s + 3, "NTFS", 4) == 0 ? "ntfs" : "ukjent";
            }
        }
        sdspi_host_remove_device(dev);
    }
    sdspi_host_deinit();
    heap_caps_free(s);
    return what;
}

void sd_card_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

void sd_card_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void sd_card_unlock(void)
{
    xSemaphoreGive(s_lock);
}

esp_err_t sd_card_ready(void)
{
    /* Still there? One short command; a card taken out answers nothing. */
    if (s_card != NULL && sdmmc_get_status(s_card) == ESP_OK) {
        return ESP_OK;
    }
    if (s_card != NULL) {
        sd_card_failed();
    }
    const int64_t now = esp_timer_get_time();
    if (s_tried && now - s_tried_us < (int64_t)SD_RETRY_MS * 1000) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_bus) {
        const spi_bus_config_t bus = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
            s_tried = true;
            s_tried_us = now;
            return ESP_ERR_NOT_FOUND;
        }
        s_bus = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_HOST;
    slot.gpio_cs = SDSPI_SLOT_NO_CS;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = SDSPI_SLOT_NO_WP;
    slot.gpio_int = SDSPI_SLOT_NO_INT;
    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false, /* never wipe someone's card */
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_CARD_ROOT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        if (!s_tried) {
            ESP_LOGW(TAG, "No card mounted: %s", esp_err_to_name(err));
        }
        /* ESP_FAIL: the card answered, its file system didn't mount. Looked
         * into once per card, not every try. */
        if (err == ESP_FAIL && s_unreadable == NULL) {
            s_unreadable = what_is_on(&host, &slot);
            ESP_LOGW(TAG, "Card can't be read: %s", s_unreadable);
        } else if (err != ESP_FAIL) {
            s_unreadable = NULL;
        }
        s_tried = true;
        s_tried_us = now;
        return ESP_ERR_NOT_FOUND;
    }
    s_card = card;
    s_tried = false;
    s_unreadable = NULL;
    snprintf(s_drive, sizeof(s_drive), "%u:", (unsigned)ff_diskio_get_pdrv_card(card));
    ESP_LOGI(TAG, "Card mounted: %s, %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

void sd_card_failed(void)
{
    if (s_card == NULL) {
        return;
    }
    ESP_LOGW(TAG, "Card failed; unmounting");
    esp_vfs_fat_sdcard_unmount(SD_CARD_ROOT, s_card);
    s_card = NULL;
    s_tried = false; /* try again next time, it may be back */
}

esp_err_t sd_card_space(uint64_t *total, uint64_t *free_bytes)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(SD_CARD_ROOT, total, free_bytes);
}

const char *sd_card_drive(void)
{
    return s_drive;
}

const char *sd_card_unreadable(void)
{
    return s_card == NULL ? s_unreadable : NULL;
}
