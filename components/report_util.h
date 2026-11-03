#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Error reports (main/report.c, server/feil/feil.py): the parts both ends
 * must agree on. */

/* What the display signs for a report sent at `t` (Unix time) whose body
 * has the SHA-256 `body_sha`: "vaertavla-feil-1\n<t>\n<hex of the hash>". */
#define REPORT_SIGNED_MAX 104
void report_signed_text(int64_t t, const uint8_t body_sha[32], char out[REPORT_SIGNED_MAX]);

/* The boundary of a multipart/form-data Content-Type into out (1 to 70 of
 * the characters RFC 2046 allows, quotes taken off); false if it isn't one. */
bool report_boundary(const char *content_type, char *out, size_t out_len);
