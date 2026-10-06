#ifndef _FORM_UTIL_H_
#define _FORM_UTIL_H_

/* Reading an application/x-www-form-urlencoded body (the setup page's form). */

#include <stdbool.h>
#include <stddef.h>

/* The value of `key` in `body`, URL-decoded into dst (at most dst_len - 1
 * bytes, cut at a whole UTF-8 character). False, and dst empty, if the key
 * isn't there. */
bool form_field(const char *body, const char *key, char *dst, size_t dst_len);

/* Append printf-style text at `p` in a buffer ending at `end`, cut short
 * there if it doesn't fit, and return where the text now ends (at most
 * end - 1, always NUL-terminated). Unlike p += snprintf(p, end - p, ...),
 * a cut never leaves p past end, where the next call's size would wrap
 * around and write beyond the buffer. */
char *buf_append(char *p, char *end, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
