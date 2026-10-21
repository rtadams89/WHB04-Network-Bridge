/*
 * USB host side: finds the WHB04 dongle, streams its input reports out and
 * writes feature reports (LCD updates) to it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define USB_PENDANT_MAX_REPORT 64

typedef struct {
    /* Called from the USB HID driver task for every input report. Must not block. */
    void (*on_input_report)(const uint8_t *data, size_t len);
    /* Called when the pendant dongle is attached or removed. */
    void (*on_status)(bool attached);
} usb_pendant_callbacks_t;

typedef struct {
    uint32_t input_reports;
    uint32_t feature_reports;
    uint32_t feature_errors;
    uint32_t feature_dropped;
    uint16_t vid;
    uint16_t pid;
} usb_pendant_stats_t;

esp_err_t usb_pendant_start(const usb_pendant_callbacks_t *callbacks);

bool usb_pendant_attached(void);

/*
 * Queue a feature report for the dongle. `data[0]` is the report ID (hidapi
 * convention; 0 means "no report IDs" and is stripped). Never blocks; returns
 * false if the report was dropped (no dongle or queue full).
 */
bool usb_pendant_send_feature(const uint8_t *data, size_t len);

void usb_pendant_get_stats(usb_pendant_stats_t *out);
