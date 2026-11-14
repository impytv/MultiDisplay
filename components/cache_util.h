#pragma once

/* Pure helpers for the memory card's cache (components/sd_cache.c): the
 * file a key is kept in, the header that says what a file holds, and which
 * files to remove when there are too many. No ESP-IDF, so they are
 * host-tested (test/host/test_cache.c). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A key (an address, or what a radar frame was made from) up to this long,
 * without its '\0'. */
#define CACHE_KEY_MAX 511

/* A file's header: "MDC1", when it was saved (epoch seconds, 0 = clock not
 * set), the data's length and CRC-32, and the key's length (u16 + u16 0),
 * all little-endian; then the key, then the data. */
#define CACHE_HEAD_FIXED 20
#define CACHE_HEAD_MAX   (CACHE_HEAD_FIXED + CACHE_KEY_MAX)

/* The name of the file `key` is kept in, "1A2B3C4D.BIN" (8.3, so FatFs
 * keeps one directory entry for it), into out (at least 13 bytes). Two keys
 * may share one: the key in the header tells them apart. */
void cache_file_name(const char *key, char *out);

/* The header for `key` and data of `len` bytes with CRC `crc`, into out
 * (CACHE_HEAD_MAX): its length, 0 if the key is too long. */
size_t cache_head_make(uint8_t *out, const char *key, uint32_t saved, uint32_t len, uint32_t crc);

/* Whether the first `n` bytes of a file of `file_len` bytes are a header
 * for `key` with the data after it: then its fields. */
bool cache_head_check(const uint8_t *head, size_t n, size_t file_len, const char *key, uint32_t *saved,
                      uint32_t *len, uint32_t *crc, size_t *head_len);

/* Whether data saved at `saved` is still to be used at `now` (epoch
 * seconds; either 0 or before `plausible` when the clock wasn't set) when
 * it is kept `max_age_s` (0 = for good). With no clock to tell, it is. */
bool cache_fresh(uint32_t saved, int64_t now, uint32_t max_age_s, int64_t plausible);

/* A file in a cache folder, as FatFs lists it: its date and time as one
 * number (later is bigger) and its size. */
typedef struct {
    uint32_t stamp;
    uint32_t size;
    char name[13];
} cache_entry_t;

/* With more than max_files, or more than max_bytes in all, in `e` (n of
 * them): sorts them oldest first and says how many from the start to
 * remove to come down to `keep_pct` percent of both. 0 if under both. */
int cache_prune_count(cache_entry_t *e, int n, int max_files, uint64_t max_bytes, int keep_pct);
