#ifndef _FORM_UTIL_H_
#define _FORM_UTIL_H_

/* Reading an application/x-www-form-urlencoded body (the setup page's form). */

#include <stdbool.h>
#include <stddef.h>

/* The value of `key` in `body`, URL-decoded into dst (at most dst_len - 1
 * bytes, cut at a whole UTF-8 character). False, and dst empty, if the key
 * isn't there. */
bool form_field(const char *body, const char *key, char *dst, size_t dst_len);

#endif
