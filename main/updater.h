#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Automatic firmware updates from the manifest at g_cfg->ota_url (see
 * docs/auto-update-plan.md). Everything runs in the weather task, between
 * its fetches. */

/* Once connected: serve /ota/status, /ota/check and /ota/install for the
 * setup page. A check or install asked for there wakes `task`, which then
 * runs it from updater_poll(). */
void updater_start(TaskHandle_t task);

/* Run a check (and install) if one is due or was asked for; lowers
 * *wait_ms to when the next one is due. Returns true if it wrote on the
 * status label, which the current screen should then put back. An install
 * that succeeds restarts the display and doesn't return. */
bool updater_poll(uint32_t *wait_ms);
