#pragma once

/* The display's own key (ECDSA P-256), made the first time and kept in NVS:
 * what the update site knows it by, for error reports (main/report.c,
 * server/feil/) and shared pictures (main/share.c, server/deling/). The
 * private half never leaves the display; the site keeps the public half,
 * and for sharing only takes a key someone has approved there. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The signature (DER, base64) of the text whose SHA-256 is `sha`, and the
 * public key (DER, base64) to check it with. Needs some 3 KB of stack.
 * False if the key can't be made or read, or it doesn't fit. */
bool display_key_sign(const uint8_t sha[32], char *key_b64, size_t key_len, char *sig_b64, size_t sig_len);

/* The key's id as the site shows it: the first 16 hex digits of the
 * SHA-256 of its public half (DER), into out (17). False without a key. */
bool display_key_id(char out[17]);
