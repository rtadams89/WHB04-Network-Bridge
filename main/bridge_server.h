/*
 * TCP server that relays pendant HID reports to one controller client.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool client_connected;
    char client_addr[48];
    uint32_t clients_accepted;
    uint32_t frames_sent;
    uint32_t frames_dropped;
} bridge_server_stats_t;

esp_err_t bridge_server_start(uint16_t port);

/* Forward an input report to the client (if any). Never blocks. */
void bridge_server_post_input(const uint8_t *data, size_t len);

/* Notify the client that the dongle was attached/removed. */
void bridge_server_post_status(bool attached);

void bridge_server_get_stats(bridge_server_stats_t *out);
