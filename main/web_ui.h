/*
 * Web UI: status page, WiFi/hostname setup (also serves the captive portal)
 * and firmware upload.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t web_ui_start(uint16_t tcp_port);
