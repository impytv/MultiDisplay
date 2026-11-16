#pragma once

/* Pure helpers for sharing pictures between displays (main/share.c and
 * server/deling/deling.py): the text a request is signed over, a shared
 * picture's id and its name on the memory card, and the groups a display
 * downloads from. No ESP-IDF, so they are host-tested (test/host). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A shared picture's id on the update site: 12 lower-case hex digits. */
#define SHARE_ID_LEN 12

/* A group's name: 1-24 of a-z, 0-9, '-' and '_'. */
#define SHARE_GROUP_MAX 25

/* The groups a display downloads from (app_config_t.share_groups), as
 * "familien,venner"; "" = all it is a member of. */
#define SHARE_GROUPS_MAX 64

/* A display's nickname (app_config_t.share_nick), shown with the
 * pictures it shares on the other displays' /bilder pages: at most 40
 * bytes of UTF-8. */
#define SHARE_NICK_MAX 41

/* The text a request is signed over (deling.py's signed_text):
 * "vaertavla-deling-1\n<method>\n<path>\n<time>\n<nonce>\n<hex SHA-256 of
 * the body>", where path is what follows ".../deling/" ("bilder?grupper=a").
 * Its length, or 0 if it doesn't fit in out_len. */
size_t share_signed_text(const char *method, const char *path, int64_t t, const char *nonce,
                         const uint8_t body_sha[32], char *out, size_t out_len);

/* Whether `id` is a shared picture's id. */
bool share_id_ok(const char *id);

/* Whether `g` is a group's name. */
bool share_group_ok(const char *g);

/* The card's name for shared picture `id`: "D<id>.JPG", into out (at least
 * SHARE_ID_LEN + 6). */
void share_file_name(const char *id, char *out, size_t out_len);

/* The id in a card name of share_file_name's ("Da1b2c3d4e5f6.JPG", any
 * case), into id (SHARE_ID_LEN + 1, lower case); false for any other name. */
bool share_file_id(const char *name, char *id);

/* What was typed for the groups to download from, made into
 * "familien,venner" in out (SHARE_GROUPS_MAX): lower case, separated by
 * commas, spaces or semicolons, each a group's name, none twice. False if
 * one isn't a group's name or it doesn't fit (out is then ""). */
bool share_groups_clean(const char *in, char *out, size_t out_len);

/* `in` %-encoded for a query (all but A-Z a-z 0-9 - _ . ~), into out.
 * False, and out "", if it doesn't fit. */
bool share_url_encode(const char *in, char *out, size_t out_len);

/* A nickname as typed made fit to keep and send, into out (at most
 * SHARE_NICK_MAX): no control characters, spaces at the ends dropped and
 * runs of them made one, cut on a whole UTF-8 character. */
void share_nick_clean(const char *in, char *out, size_t out_len);

/* A line of the card's list of who shared which picture, "<id>\t<nick>",
 * into id (SHARE_ID_LEN + 1) and nick (SHARE_NICK_MAX); false if it isn't
 * one. */
bool share_nick_line(const char *line, char *id, char *nick);
