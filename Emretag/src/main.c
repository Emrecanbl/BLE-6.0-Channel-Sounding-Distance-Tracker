/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief EmreTag - Channel Sounding reflector with Ranging Responder.
 *
 *  Only the start-up order lives here. Drivers sit in imu.c, button.c and
 *  outputs.c; the link and the Channel Sounding role in ble_cs.c; the
 *  application - the profile and what the tag does - in app_gatt.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_gatt.h"
#include "ble_cs.h"
#include "outputs.h"

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

int main(void)
{
	int err;

	LOG_INF("Starting EmreTag - Channel Sounding reflector");

	err = outputs_init();
	if (err) {
		return -1;
	}

	/* Selects the antenna path before bringing the radio up. */
	err = ble_cs_start();
	if (err) {
		return -1;
	}

	/* After advertising is up: from here a button press or a tap can reach
	 * the Bluetooth stack.
	 */
	err = app_gatt_init();
	if (err) {
		return -1;
	}

	ble_cs_run();

	return 0;
}
