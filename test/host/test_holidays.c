/* The holiday table and week numbers for the month calendar. */

#include <stddef.h>

#include "check.h"
#include "holidays.h"

void test_holidays(void)
{
    CHECK_STR(holiday_name(2026, 12, 25), "1. juledag");
    CHECK_STR(holiday_name(2026, 5, 17), "Grunnlovsdag");
    CHECK_STR(holiday_name(2027, 3, 26), "Langfredag");
    CHECK_STR(holiday_name(2028, 5, 25), "Kristi Himmelfartsdag");
    CHECK_STR(holiday_name(2029, 4, 2), "2. p\xC3\xA5skedag");
    CHECK(holiday_name(2026, 10, 14) == NULL);
    CHECK(holiday_name(2026, 10, 18) == NULL); /* a plain Sunday isn't in it */
    CHECK(holiday_name(2030, 12, 25) == NULL); /* past the table */
    CHECK(holidays_known(2026) && holidays_known(2029));
    CHECK(!holidays_known(2025) && !holidays_known(2030));

    CHECK_INT(iso_week(2026, 10, 14), 42);
    CHECK_INT(iso_week(2026, 1, 1), 1);    /* a Thursday */
    CHECK_INT(iso_week(2027, 1, 1), 53);   /* a Friday: the last week of 2026 */
    CHECK_INT(iso_week(2026, 12, 28), 53);
    CHECK_INT(iso_week(2027, 1, 4), 1);
    CHECK_INT(iso_week(2024, 12, 30), 1);  /* a Monday: week 1 of 2025 */
    CHECK_INT(iso_week(2028, 1, 2), 52);   /* a Sunday */
    CHECK_INT(iso_week(2028, 1, 3), 1);
    CHECK_INT(iso_week(2029, 12, 31), 1);
}
