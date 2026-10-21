/*
 * Persistent settings (NVS) with compile-time defaults from Kconfig.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_SSID_MAX     32
#define SETTINGS_PASSWORD_MAX 64
#define SETTINGS_HOSTNAME_MAX 32

typedef struct {
    char ssid[SETTINGS_SSID_MAX + 1];
    char password[SETTINGS_PASSWORD_MAX + 1];
    char hostname[SETTINGS_HOSTNAME_MAX + 1];
    bool wifi_from_nvs; /* false: ssid/password came from Kconfig (or are empty) */
} bridge_settings_t;

/* Initialise NVS and load settings. Must be called first. */
esp_err_t settings_init(void);

/* Current settings (loaded at boot). */
const bridge_settings_t *settings_get(void);

/* Save WiFi credentials and hostname. Takes effect after reboot. */
esp_err_t settings_save(const char *ssid, const char *password, const char *hostname);

/* Erase saved WiFi credentials (falls back to Kconfig defaults / portal). */
esp_err_t settings_clear_wifi(void);

/* true if hostname is 1-32 chars of [a-z0-9-], not starting/ending with '-' */
bool settings_hostname_valid(const char *hostname);
