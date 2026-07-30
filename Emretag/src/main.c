/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief EmreTag - Channel Sounding reflector with Ranging Responder.
 *
 *  Start-up order and the wiring between modules live here; the modules
 *  themselves know nothing about each other.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>

#include "app_gatt.h"
#include "ble_cs.h"
#include "button.h"
#include "outputs.h"

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

/*
 * XIAO nRF54L15 RF path selection.
 *
 * The board routes the radio through an RF switch that selects between the
 * onboard ceramic antenna and the external u.FL connector. Nothing enables the
 * switch at start-up, and upstream samples (including the Channel Sounding
 * ones) are unaware of it, which leaves the RF path undefined and cripples the
 * link budget.
 *
 * Measured on this board: enabling the switch and selecting the ceramic
 * antenna extended usable ranging distance from ~2-3 m to >12 m.
 *
 *   rfsw_pwr : powers the RF switch (must stay enabled)
 *   rfsw_ctl : path select - LOW = ceramic antenna, HIGH = external u.FL
 *
 * Set rfsw_ctl HIGH only when an antenna is attached to the u.FL connector.
 * This must run before the Bluetooth stack is started.
 */
static const struct device *const rfsw_pwr = DEVICE_DT_GET(DT_NODELABEL(rfsw_pwr));
static const struct device *const rfsw_ctl = DEVICE_DT_GET(DT_NODELABEL(rfsw_ctl));

static void select_ceramic_antenna(void)
{
	regulator_enable(rfsw_pwr);
	k_msleep(1);		/* let the switch supply settle */
	regulator_enable(rfsw_ctl);
}

/* Runs on the system work queue, so blocking Bluetooth calls are fine here. */
static void on_button_change(uint8_t state, uint16_t press_count)
{
	int err;

	err = app_gatt_notify_button(state);
	if (err) {
		LOG_WRN("Failed to notify button state (err %d)", err);
	}

	err = ble_cs_set_press_count(press_count);
	if (err) {
		LOG_WRN("Failed to update advertising data (err %d)", err);
	}
}

int main(void)
{
	int err;

	LOG_INF("Starting EmreTag - Channel Sounding reflector");

	err = outputs_init();
	if (err) {
		LOG_ERR("Output init failed (err %d)", err);
		return -1;
	}

	select_ceramic_antenna();

	err = ble_cs_start();
	if (err) {
		return -1;
	}

	/* Last, so that a button press can never reach the Bluetooth stack
	 * before advertising is running.
	 */
	err = button_init(on_button_change);
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return -1;
	}

	ble_cs_run();

	return 0;
}
