/*
 * WHB04 Network Bridge wire protocol (version 1).
 *
 * The bridge is the TCP server; the CNC controller application is the client.
 * Every message is a frame: [type:1][length:1][payload:length]. See
 * docs/PROTOCOL.md for the full description.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define BRIDGE_PROTOCOL_VERSION 1

/* Bridge -> client */
#define FRAME_HELLO          0x01 /* [protocol_version, firmware_version...] */
#define FRAME_STATUS         0x02 /* [dongle_attached (0/1)] */
#define FRAME_INPUT_REPORT   0x03 /* raw HID input report incl. report ID */
#define FRAME_HEARTBEAT      0x04 /* empty */

/* Client -> bridge */
#define FRAME_FEATURE_REPORT 0x81 /* raw HID feature report, first byte = report ID */
#define FRAME_CLIENT_HEARTBEAT 0x82 /* empty */

#define FRAME_MAX_PAYLOAD 255

/* Bridge sends a heartbeat if it has sent nothing else for this long. */
#define BRIDGE_HEARTBEAT_INTERVAL_MS 500
/* Bridge drops a client it has heard nothing from for this long. */
#define BRIDGE_CLIENT_TIMEOUT_MS 3000
