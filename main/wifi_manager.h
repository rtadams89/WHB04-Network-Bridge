/*
 * WiFi station + fallback setup portal (soft AP).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_STATE_CONNECTING, /* trying to join the configured network */
    WIFI_STATE_CONNECTED,  /* station has an IP address */
    WIFI_STATE_PORTAL,     /* setup access point active, station not connected */
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    bool portal_active;
    char ssid[33];
    char ip[16];
    char ap_ssid[33];
    int8_t rssi;
} wifi_status_t;

/* Called (from the event task) whenever the station gets an IP address. */
typedef void (*wifi_connected_cb_t)(void);

esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected);

void wifi_manager_get_status(wifi_status_t *out);

/*
 * Scan for networks. Writes a JSON array of {"ssid":..,"rssi":..,"secure":..}
 * objects into `out`. Blocks for a few seconds.
 */
esp_err_t wifi_manager_scan_json(char *out, size_t out_size);
