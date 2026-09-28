#ifndef _OTA_WRITER_H_
#define _OTA_WRITER_H_

/* Writes a firmware image into the app slot not running, for both the setup
 * page's upload (wifi_provision.c) and the automatic updater (updater.c), so
 * the checks on what gets installed live in one place:
 *
 *  - the image must be this project's firmware (the app description at its
 *    start names the project), and fit the slot;
 *  - esp_ota_end() then verifies the whole image, including its signature:
 *    with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT only an image signed
 *    with the same key as the running firmware passes (see
 *    docs/auto-update-plan.md).
 *
 * Only once all that passed does the slot become the one booted next. The
 * functions return NULL on success, or a short English message saying what
 * went wrong (the writer is then finished with, and nothing was selected). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

/* Enough of the image to hold its app description: the image header, the
 * first segment's header and the description itself. */
#define OTA_WRITER_HEAD (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

typedef struct {
    const esp_partition_t *part;
    esp_ota_handle_t handle; /* 0 until the head has been checked */
    size_t total;            /* the image's size, if known in advance (else 0) */
    size_t written;
    uint8_t head[OTA_WRITER_HEAD];
    size_t head_len;
    esp_app_desc_t app;      /* valid once ota_writer_app() is non-NULL */
    mbedtls_sha256_context sha;
    bool active;
} ota_writer_t;

/* Start an image of `total` bytes (0 = not known yet). */
const char *ota_writer_start(ota_writer_t *w, size_t total);

/* Add the next `n` bytes of the image, in any size of pieces. */
const char *ota_writer_write(ota_writer_t *w, const void *data, size_t n);

/* The new image's app description (project, version, build time), once
 * enough of it has been written and it has passed the project check; else
 * NULL. */
const esp_app_desc_t *ota_writer_app(const ota_writer_t *w);

/* All written: verify it (signature included) and select it for the next
 * boot. With `expect_sha256` (else NULL), the SHA-256 of the bytes written
 * must also equal it. */
const char *ota_writer_finish(ota_writer_t *w, const uint8_t expect_sha256[32]);

/* Give up on the image (safe to call at any point, also after a failure). */
void ota_writer_abort(ota_writer_t *w);

#endif
