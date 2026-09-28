#ifndef _VERSION_UTIL_H_
#define _VERSION_UTIL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "1.10.2" (anything after a '-' ignored) into major/minor/patch; false if
 * it doesn't start with a number. */
bool version_parse(const char *s, int v[3]);

/* <0, 0, >0 as a is older than, the same as, newer than b; an unparsable
 * version is never newer. */
int version_compare(const char *a, const char *b);

/* `ref` made absolute against `base` (a URL): kept if it has a scheme,
 * against base's host if it starts with '/', else against base's folder. */
void url_resolve(const char *base, const char *ref, char *out, size_t out_len);

/* 64 hex digits into 32 bytes; false if it isn't exactly that. */
bool hex_to_bytes32(const char *hex, uint8_t out[32]);

#endif
