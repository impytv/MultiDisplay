/* The holidays and week numbers for the month calendar. */

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
    /* From Easter, any year: the earliest and latest it can be, and past
     * 2029 (where the table they came from ended). */
    CHECK_STR(holiday_name(2030, 4, 21), "1. p\xC3\xA5skedag");
    CHECK_STR(holiday_name(2030, 12, 25), "1. juledag");
    CHECK_STR(holiday_name(2035, 3, 25), "1. p\xC3\xA5skedag");
    CHECK_STR(holiday_name(2035, 3, 18), "Palmes\xC3\xB8ndag");
    CHECK_STR(holiday_name(2038, 4, 25), "1. p\xC3\xA5skedag");
    CHECK_STR(holiday_name(2038, 6, 14), "2. pinsedag");
    CHECK_STR(holiday_name(2008, 3, 23), "1. p\xC3\xA5skedag");
    CHECK(holiday_name(2285, 3, 22) == NULL);
    /* Two on one day. */
    CHECK_STR(holiday_name(2027, 5, 17), "Grunnlovsdag / 2. pinsedag");
    CHECK_STR(holiday_name(2008, 5, 1), "Offentlig h\xC3\xB8ytidsdag / Kristi Himmelfartsdag");
    CHECK_STR(holiday_name(2043, 5, 17), "Grunnlovsdag / 1. pinsedag");
    CHECK_STR(holiday_name(2091, 5, 17), "Grunnlovsdag / Kristi Himmelfartsdag");
    CHECK(holidays_known(1900) && holidays_known(2199));
    CHECK(!holidays_known(1899) && !holidays_known(2200));

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
