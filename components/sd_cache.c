/* Downloads kept on the memory card (see sd_cache.h).
 *
 * One file per key, CACHE/<kind>/<hash>.BIN, holding a header (the key,
 * when it was saved, the data's length and CRC-32; cache_util.c) and the
 * data. Written as <hash>.TMP and renamed, so a file cut short by the power
 * going is never read as whole; any file that doesn't check out is a miss.
 *
 * Without a card: a try to mount one takes a moment, so after a failed try
 * the cache leaves the card alone for CACHE_RETRY_S, unless something else
 * (the picture screen) mounts it meanwhile. Everything runs with the card's
 * lock held, which the picture screen and the /bilder page share.
 *
 * The files and bytes of each kind are counted from a listing on first use
 * after the card is mounted, and counted up as files are put; when a kind
 * passes its cap, it is listed again and the oldest (by FatFs's date and
 * time) are removed down to CACHE_KEEP_PCT. A card with under
 * CACHE_CARD_LEFT free gets nothing more until it has room again. */

#include "sd_cache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "ff.h"

#include "cache_util.h"
#include "pic_util.h"
#include "sd_card.h"

static const char *TAG = "sd_cache";

#define CACHE_DIR        SD_CARD_ROOT "/CACHE"
#define CACHE_RETRY_S    (5 * 60)  /* no card: tried again after this, then twice as long, */
#define CACHE_RETRY_MAX_S (60 * 60) /* up to this */
#define CACHE_TOUCH_S    (24 * 3600) /* a cover used dated anew if older than this */
#define CACHE_KEEP_PCT   80
#define CACHE_CARD_LEFT  (64ULL * 1024 * 1024)
#define CACHE_PLAUSIBLE  1672531200 /* the clock counts as set from this on (as PLAUSIBLE_EPOCH_S) */
#define CACHE_ROOM_LEFT  (192 * 1024) /* PSRAM left beside a file read in */
#define CACHE_RESCAN_PUTS 50          /* a full card looked at again after this many puts */

typedef struct {
    const char *dir;
    int max_files;
    uint64_t max_bytes;
} kind_info_t;

static const kind_info_t KINDS[SD_CACHE_KINDS] = {
    /* An hour of frames is 13; two locations or ranges, a few hours. */
    [SD_CACHE_RAIN] = { "RAIN", 48, 16ULL * 1024 * 1024 },
    /* 20-90 KB each: some 1000 covers. */
    [SD_CACHE_COVER] = { "COVER", 1000, 64ULL * 1024 * 1024 },
};

typedef struct {
    bool counted; /* since the card was mounted */
    int files;
    uint64_t bytes;
} kind_state_t;

static kind_state_t s_kind[SD_CACHE_KINDS];
static int64_t s_missed_us;  /* the last try to mount that failed, 0 = none */
static int s_retry_s = CACHE_RETRY_S;
static uint32_t s_mount_seen; /* sd_card_mount_count() the counts are for */
static bool s_card_full;
static int s_puts_full;
static EXT_RAM_BSS_ATTR uint8_t s_head[CACHE_HEAD_MAX];

/* The card's lock taken and the card mounted: true, and the caller unlocks
 * with end(). False (lock not held) with no card. A try to mount takes a
 * moment with no card (and holds the weather task meanwhile), so after one
 * fails the cache leaves the card alone for CACHE_RETRY_S, then twice as
 * long each time up to CACHE_RETRY_MAX_S - unless the picture screen
 * mounts one meanwhile. */
static bool begin(void)
{
    sd_card_lock();
    const int64_t now = esp_timer_get_time();
    if (!sd_card_mounted() && s_missed_us != 0 && now - s_missed_us < (int64_t)s_retry_s * 1000000) {
        sd_card_unlock();
        return false;
    }
    if (sd_card_ready() != ESP_OK) {
        if (s_missed_us != 0) {
            s_retry_s = s_retry_s * 2 < CACHE_RETRY_MAX_S ? s_retry_s * 2 : CACHE_RETRY_MAX_S;
        }
        s_missed_us = now;
        sd_card_unlock();
        return false;
    }
    s_missed_us = 0;
    s_retry_s = CACHE_RETRY_S;
    if (sd_card_mount_count() != s_mount_seen) {
        /* Mounted again, perhaps another card: counted afresh. */
        memset(s_kind, 0, sizeof(s_kind));
        s_card_full = false;
        s_puts_full = 0;
        s_mount_seen = sd_card_mount_count();
    }
    return true;
}

/* A file of `size` bytes gone from kind's folder. */
static void uncount(sd_cache_kind_t kind, size_t size)
{
    kind_state_t *st = &s_kind[kind];
    if (st->counted && st->files > 0) {
        st->files--;
        st->bytes -= size < st->bytes ? size : st->bytes;
    }
}

static void end(void)
{
    sd_card_unlock();
}

static void path_for(sd_cache_kind_t kind, const char *key, const char *ext, char *out, size_t out_len)
{
    char name[13];
    cache_file_name(key, name);
    snprintf(out, out_len, CACHE_DIR "/%s/%.8s.%s", KINDS[kind].dir, name, ext);
}

/* List kind's folder, removing what a cut-short write left (.TMP) and, if
 * over its cap, the oldest; and see whether the card has room. */
static void count_and_prune(sd_cache_kind_t kind)
{
    const kind_info_t *k = &KINDS[kind];
    kind_state_t *st = &s_kind[kind];
    const int cap = k->max_files * 2;
    cache_entry_t *e = heap_caps_malloc((size_t)cap * sizeof(*e), MALLOC_CAP_SPIRAM);
    FF_DIR *dir = heap_caps_malloc(sizeof(FF_DIR), MALLOC_CAP_SPIRAM);
    FILINFO *fi = heap_caps_malloc(sizeof(FILINFO), MALLOC_CAP_SPIRAM);
    char fpath[24];
    snprintf(fpath, sizeof(fpath), "%s/CACHE/%s", sd_card_drive(), k->dir);
    int n = 0;
    if (e && dir && fi && f_opendir(dir, fpath) == FR_OK) {
        while (f_readdir(dir, fi) == FR_OK && fi->fname[0] != '\0') {
            if (fi->fattrib & AM_DIR) {
                continue;
            }
            const size_t nl = strlen(fi->fname);
            if (nl == 12 && strcmp(fi->fname + 8, ".TMP") == 0) {
                char p[48];
                snprintf(p, sizeof(p), "%s/%s", fpath, fi->fname);
                f_unlink(p);
            } else if (n < cap && nl == 12) {
                e[n].stamp = (uint32_t)fi->fdate << 16 | fi->ftime;
                e[n].size = fi->fsize > UINT32_MAX ? UINT32_MAX : (uint32_t)fi->fsize;
                memcpy(e[n].name, fi->fname, 13);
                n++;
            }
        }
        f_closedir(dir);
    }
    int removed = 0;
    if (e) {
        removed = cache_prune_count(e, n, k->max_files, k->max_bytes, CACHE_KEEP_PCT);
        for (int i = 0; i < removed; i++) {
            char p[48];
            snprintf(p, sizeof(p), "%s/%s", fpath, e[i].name);
            f_unlink(p);
        }
        st->files = n - removed;
        st->bytes = 0;
        for (int i = removed; i < n; i++) {
            st->bytes += e[i].size;
        }
    }
    st->counted = e != NULL;
    heap_caps_free(e);
    heap_caps_free(dir);
    heap_caps_free(fi);
    uint64_t total = 0, free_b = 0;
    s_card_full = sd_card_space(&total, &free_b) != ESP_OK || free_b < CACHE_CARD_LEFT;
    s_puts_full = 0;
    if (removed > 0 || s_card_full) {
        ESP_LOGI(TAG, "%s: %d files, %llu KB%s; %d removed", k->dir, st->files,
                 (unsigned long long)(st->bytes / 1024), s_card_full ? ", card full" : "", removed);
    }
}

/* The file for `key` (at `path`, 48 bytes, filled in), checked: open at
 * the data, with its length, CRC and FatFs date. */
static FILE *open_checked(sd_cache_kind_t kind, const char *key, uint32_t max_age_s, char *path, size_t *len,
                          uint32_t *crc, time_t *mtime)
{
    path_for(kind, key, "BIN", path, 48);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    struct stat sb;
    uint32_t saved = 0, l = 0;
    size_t hl = 0;
    const size_t got = fstat(fileno(f), &sb) == 0 ? fread(s_head, 1, sizeof(s_head), f) : 0;
    if (got == 0 || !cache_head_check(s_head, got, (size_t)sb.st_size, key, &saved, &l, crc, &hl) ||
        fseek(f, (long)hl, SEEK_SET) != 0) {
        fclose(f);
        return NULL; /* another key's (same hash), or not whole */
    }
    if (!cache_fresh(saved, time(NULL), max_age_s, CACHE_PLAUSIBLE)) {
        fclose(f);
        if (unlink(path) == 0) { /* to be fetched again */
            uncount(kind, (size_t)sb.st_size);
        }
        return NULL;
    }
    *len = l;
    *mtime = sb.st_mtime;
    return f;
}

/* A cover read: dated anew now and then, so the ones used most are the
 * last to be removed for room, not the first written. */
static void touch(sd_cache_kind_t kind, const char *path, time_t mtime)
{
    const time_t now = time(NULL);
    if (kind == SD_CACHE_COVER && now > CACHE_PLAUSIBLE && now - mtime > CACHE_TOUCH_S) {
        utime(path, NULL);
    }
}

static bool read_body(FILE *f, void *buf, size_t len, uint32_t crc)
{
    const bool ok = fread(buf, 1, len, f) == len && esp_rom_crc32_le(0, buf, len) == crc;
    fclose(f);
    return ok;
}

bool sd_cache_get(sd_cache_kind_t kind, const char *key, uint32_t max_age_s, size_t max, uint8_t **data,
                  size_t *len)
{
    if (!begin()) {
        return false;
    }
    size_t l = 0;
    uint32_t crc = 0;
    uint8_t *buf = NULL;
    char path[48];
    time_t mtime = 0;
    FILE *f = open_checked(kind, key, max_age_s, path, &l, &crc, &mtime);
    if (f != NULL) {
        if (l + 1 <= max && l + CACHE_ROOM_LEFT <= heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)) {
            buf = heap_caps_malloc(l + 1, MALLOC_CAP_SPIRAM);
        }
        if (buf == NULL) {
            fclose(f);
        } else if (!read_body(f, buf, l, crc)) {
            ESP_LOGW(TAG, "%s: kept copy unreadable", KINDS[kind].dir);
            heap_caps_free(buf);
            buf = NULL;
        } else {
            buf[l] = '\0';
            touch(kind, path, mtime);
        }
    }
    end();
    if (buf == NULL) {
        return false;
    }
    *data = buf;
    *len = l;
    return true;
}

bool sd_cache_get_into(sd_cache_kind_t kind, const char *key, uint32_t max_age_s, void *buf, size_t len)
{
    if (!begin()) {
        return false;
    }
    size_t l = 0;
    uint32_t crc = 0;
    char path[48];
    time_t mtime = 0;
    FILE *f = open_checked(kind, key, max_age_s, path, &l, &crc, &mtime);
    bool ok = false;
    if (f != NULL && l != len) {
        fclose(f);
    } else if (f != NULL) {
        ok = read_body(f, buf, len, crc);
        if (ok) {
            touch(kind, path, mtime);
        }
    }
    end();
    return ok;
}

/* fopen for writing, making CACHE/<kind> if it isn't there. */
static FILE *create(sd_cache_kind_t kind, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL && errno == ENOENT) {
        char dir[32];
        mkdir(CACHE_DIR, 0777);
        snprintf(dir, sizeof(dir), CACHE_DIR "/%s", KINDS[kind].dir);
        mkdir(dir, 0777);
        f = fopen(path, "wb");
    }
    return f;
}

void sd_cache_put(sd_cache_kind_t kind, const char *key, const void *data, size_t len)
{
    const size_t kl = strlen(key);
    if (len == 0 || len > UINT32_MAX / 2 || kl == 0 || kl > CACHE_KEY_MAX) {
        return;
    }
    const time_t now = time(NULL);
    const uint32_t crc = esp_rom_crc32_le(0, data, len); /* before the lock: others needn't wait */
    if (!begin()) {
        return;
    }
    kind_state_t *st = &s_kind[kind];
    if (!st->counted || (s_card_full && ++s_puts_full >= CACHE_RESCAN_PUTS)) {
        count_and_prune(kind);
    }
    if (s_card_full) {
        end();
        return;
    }
    char tmp[48], path[48];
    path_for(kind, key, "TMP", tmp, sizeof(tmp));
    path_for(kind, key, "BIN", path, sizeof(path));
    const size_t hl = cache_head_make(s_head, key, now > CACHE_PLAUSIBLE ? (uint32_t)now : 0, (uint32_t)len, crc);
    FILE *f = create(kind, tmp);
    bool ok = f != NULL && fwrite(s_head, 1, hl, f) == hl && fwrite(data, 1, len, f) == len;
    if (f != NULL) {
        ok &= fclose(f) == 0;
    }
    if (ok) {
        struct stat old;
        if (stat(path, &old) == 0 && unlink(path) == 0) {
            uncount(kind, (size_t)old.st_size); /* replaced */
        }
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
        ESP_LOGW(TAG, "%s: couldn't keep %u bytes", KINDS[kind].dir, (unsigned)len);
        /* Full, perhaps: looked at again before the next. */
        st->counted = false;
    } else {
        st->files++;
        st->bytes += hl + len;
        if (st->files > KINDS[kind].max_files || st->bytes > KINDS[kind].max_bytes) {
            count_and_prune(kind);
        }
    }
    end();
}

void sd_cache_drop(sd_cache_kind_t kind, const char *key)
{
    if (!begin()) {
        return;
    }
    char path[48];
    path_for(kind, key, "BIN", path, sizeof(path));
    struct stat old;
    if (stat(path, &old) == 0 && unlink(path) == 0) {
        uncount(kind, (size_t)old.st_size);
    }
    end();
}

esp_err_t sd_cache_http_get(sd_cache_kind_t kind, const char *url, uint32_t max_age_s, const char *user_agent,
                            int timeout_ms, bool verify, http_buf_t *b, const char *tag, bool *from_card)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (from_card) {
        *from_card = false;
    }
    if (sd_cache_get(kind, url, max_age_s, b->max, &data, &len)) {
        http_buf_free(b);
        b->buf = (char *)data;
        b->len = len;
        b->cap = len + 1;
        ESP_LOGD(TAG, "%u bytes from the card: %.120s", (unsigned)len, url);
        if (from_card) {
            *from_card = true;
        }
        return ESP_OK;
    }
    return sd_cache_http_fetch(kind, url, user_agent, timeout_ms, verify, b, tag);
}

esp_err_t sd_cache_http_fetch(sd_cache_kind_t kind, const char *url, const char *user_agent, int timeout_ms,
                              bool verify, http_buf_t *b, const char *tag)
{
    const esp_err_t err = http_get_buf(url, user_agent, timeout_ms, verify, b, tag);
    /* Only a picture, or a sheet of logos (sonos_logo_find's "RLG1"): not
     * an error page some server sent with 200, kept for a month. */
    const uint8_t *d = (const uint8_t *)b->buf;
    if (err == ESP_OK && d != NULL && b->len > 0 &&
        (kind != SD_CACHE_COVER || pic_looks_like_picture(d, b->len) || (b->len > 8 && memcmp(d, "RLG1", 4) == 0))) {
        sd_cache_put(kind, url, b->buf, b->len);
    }
    return err;
}
