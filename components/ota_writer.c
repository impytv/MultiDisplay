#include <string.h>

#include "esp_log.h"
#include "spi_flash_mmap.h"

#include "ota_writer.h"

static const char *TAG = "ota_writer";

static const char *fail(ota_writer_t *w, const char *msg)
{
    ESP_LOGE(TAG, "Firmware update: %s", msg);
    ota_writer_abort(w);
    return msg;
}

const char *ota_writer_start(ota_writer_t *w, size_t total)
{
    memset(w, 0, sizeof(*w));
    w->part = esp_ota_get_next_update_partition(NULL);
    if (w->part == NULL) {
        return fail(w, "Ingen plass til oppdateringen (skriv OTA-partisjonstabellen over USB f\xC3\xB8" "rst)");
    }
    if (total != 0 && (total < OTA_WRITER_HEAD || total > w->part->size)) {
        return fail(w, "Det ser ikke ut som programvare av riktig st\xC3\xB8" "rrelse");
    }
    w->total = total;
    mbedtls_sha256_init(&w->sha);
    mbedtls_sha256_starts(&w->sha, 0);
    w->active = true;
    return NULL;
}

/* The head is complete: refuse anything that isn't this project's firmware,
 * then start writing the slot. */
static const char *check_head(ota_writer_t *w)
{
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(w->head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (w->head[0] != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0) {
        return fail(w, "Ikke programvare for MultiDisplay");
    }
    memcpy(&w->app, d, sizeof(w->app));
    ESP_LOGI(TAG, "Firmware update: version %.32s, built %.16s %.16s, into %s", d->version, d->date, d->time,
             w->part->label);
    /* With the size known, erase it all first, plus the sector after it:
     * that is where an unsigned image's signature would be looked for, and a
     * signature left there by an earlier image must not be read as this
     * one's. (Sequential writes only erase what they reach.) */
    size_t erase = OTA_WITH_SEQUENTIAL_WRITES;
    if (w->total != 0) {
        erase = (w->total + 2 * SPI_FLASH_SEC_SIZE - 1) / SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE;
        if (erase > w->part->size) {
            erase = w->part->size;
        }
    }
    if (esp_ota_begin(w->part, erase, &w->handle) != ESP_OK) {
        w->handle = 0;
        return fail(w, "Kunne ikke begynne \xC3\xA5" " skrive oppdateringen");
    }
    if (esp_ota_write(w->handle, w->head, w->head_len) != ESP_OK) {
        return fail(w, "Skrivingen av oppdateringen feilet");
    }
    return NULL;
}

const char *ota_writer_write(ota_writer_t *w, const void *data, size_t n)
{
    if (!w->active) {
        return "Oppdateringen ble avbrutt";
    }
    if (w->written + n > w->part->size || (w->total != 0 && w->written + n > w->total)) {
        return fail(w, "Filen er st\xC3\xB8" "rre enn den skal v\xC3\xA6" "re");
    }
    mbedtls_sha256_update(&w->sha, data, n);
    w->written += n;
    const uint8_t *p = data;
    if (w->handle == 0) {
        const size_t take = (n < OTA_WRITER_HEAD - w->head_len) ? n : OTA_WRITER_HEAD - w->head_len;
        memcpy(w->head + w->head_len, p, take);
        w->head_len += take;
        p += take;
        n -= take;
        if (w->head_len < OTA_WRITER_HEAD) {
            return NULL;
        }
        const char *err = check_head(w);
        if (err != NULL) {
            return err;
        }
    }
    if (n > 0 && esp_ota_write(w->handle, p, n) != ESP_OK) {
        return fail(w, "Skrivingen av oppdateringen feilet");
    }
    return NULL;
}

const esp_app_desc_t *ota_writer_app(const ota_writer_t *w)
{
    return (w->active && w->handle != 0) ? &w->app : NULL;
}

const char *ota_writer_finish(ota_writer_t *w, const uint8_t expect_sha256[32])
{
    if (!w->active) {
        return "Oppdateringen ble avbrutt";
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&w->sha, digest);
    if (w->handle == 0 || (w->total != 0 && w->written != w->total)) {
        return fail(w, "Filen er ufullstendig");
    }
    if (expect_sha256 != NULL && memcmp(digest, expect_sha256, sizeof(digest)) != 0) {
        return fail(w, "Filens SHA-256 stemmer ikke med manifestet");
    }
    /* Verifies the whole image as written, signature included. */
    esp_err_t err = esp_ota_end(w->handle);
    w->handle = 0;
    if (err != ESP_OK) {
        return fail(w, err == ESP_ERR_OTA_VALIDATE_FAILED
                           ? "Filen er skadet, eller ikke signert med skjermens n\xC3\xB8" "kkel"
                           : "Kunne ikke fullf\xC3\xB8" "re oppdateringen");
    }
    if (esp_ota_set_boot_partition(w->part) != ESP_OK) {
        return fail(w, "Kunne ikke velge den nye programvaren");
    }
    mbedtls_sha256_free(&w->sha);
    w->active = false;
    ESP_LOGI(TAG, "Firmware update: %u bytes written and verified", (unsigned)w->written);
    return NULL;
}

void ota_writer_abort(ota_writer_t *w)
{
    if (w->handle != 0) {
        esp_ota_abort(w->handle);
        w->handle = 0;
    }
    if (w->active) {
        mbedtls_sha256_free(&w->sha);
        w->active = false;
    }
}
