/*
 * Tiny DNS responder for the setup portal: answers every A query with the
 * access point's address so phones pop up the "sign in to network" page.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

void captive_dns_start(void);
void captive_dns_stop(void);
