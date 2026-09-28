/* A minimal test harness: CHECK records failures and carries on. */
#pragma once
#include <stdio.h>
#include <string.h>

extern int g_checks, g_failures;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); g_checks++; \
    if (strcmp(a_, b_) != 0) { g_failures++; \
        fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); } } while (0)
#define CHECK_INT(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { g_failures++; \
        fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

void test_nvs_clear(void);
