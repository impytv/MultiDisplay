#include "cache_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

void cache_file_name(const char *key, char *out)
{
    snprintf(out, 13, "%08lX.BIN", (unsigned long)fnv1a(key));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t cache_head_make(uint8_t *out, const char *key, uint32_t saved, uint32_t len, uint32_t crc)
{
    const size_t kl = strlen(key);
    if (kl == 0 || kl > CACHE_KEY_MAX) {
        return 0;
    }
    memcpy(out, "MDC1", 4);
    put32(out + 4, saved);
    put32(out + 8, len);
    put32(out + 12, crc);
    out[16] = (uint8_t)kl;
    out[17] = (uint8_t)(kl >> 8);
    out[18] = out[19] = 0;
    memcpy(out + CACHE_HEAD_FIXED, key, kl);
    return CACHE_HEAD_FIXED + kl;
}

bool cache_head_check(const uint8_t *head, size_t n, size_t file_len, const char *key, uint32_t *saved,
                      uint32_t *len, uint32_t *crc, size_t *head_len)
{
    if (n < CACHE_HEAD_FIXED || memcmp(head, "MDC1", 4) != 0 || head[18] != 0 || head[19] != 0) {
        return false;
    }
    const size_t kl = (size_t)head[16] | (size_t)head[17] << 8;
    const size_t hl = CACHE_HEAD_FIXED + kl;
    if (kl != strlen(key) || hl > n || memcmp(head + CACHE_HEAD_FIXED, key, kl) != 0) {
        return false;
    }
    const uint32_t l = get32(head + 8);
    if (file_len < hl || file_len - hl != l || l == 0) {
        return false;
    }
    *saved = get32(head + 4);
    *len = l;
    *crc = get32(head + 12);
    *head_len = hl;
    return true;
}

bool cache_fresh(uint32_t saved, int64_t now, uint32_t max_age_s, int64_t plausible)
{
    if (max_age_s == 0 || saved < plausible || now < plausible) {
        return true;
    }
    /* Saved "in the future" (a clock set wrong then): not to be trusted. */
    return (int64_t)saved <= now + 86400 && now - (int64_t)saved <= (int64_t)max_age_s;
}

static int by_stamp(const void *a, const void *b)
{
    const cache_entry_t *x = a, *y = b;
    return x->stamp < y->stamp ? -1 : x->stamp > y->stamp ? 1 : strcmp(x->name, y->name);
}

int cache_prune_count(cache_entry_t *e, int n, int max_files, uint64_t max_bytes, int keep_pct)
{
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += e[i].size;
    }
    if (n <= max_files && total <= max_bytes) {
        return 0;
    }
    qsort(e, (size_t)n, sizeof(*e), by_stamp);
    const int files_to = (int)((int64_t)max_files * keep_pct / 100);
    const uint64_t bytes_to = max_bytes / 100 * (uint64_t)keep_pct;
    int k = 0;
    while (k < n && (n - k > files_to || total > bytes_to)) {
        total -= e[k++].size;
    }
    return k;
}
