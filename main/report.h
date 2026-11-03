#pragma once

/* POST /feil: an error report from the navigation page, signed and sent on
 * to the update site (report.c). After the web server has started. */
void report_start(void);
