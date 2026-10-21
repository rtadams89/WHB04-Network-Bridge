/*
 * Status LED patterns and the WiFi-reset button.
 *
 *   fast blink (5 Hz)     setup portal active
 *   slow blink (1 Hz)     connecting to WiFi
 *   short blip every 2 s  on WiFi, waiting for the dongle and/or a client
 *   solid on              dongle attached and controller connected
 *
 * Holding the reset button for 5 s erases the saved WiFi settings and reboots.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

void status_led_start(void);
