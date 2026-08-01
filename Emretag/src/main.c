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
#include "imu.h"
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

/* Finding the phone takes two gestures, so that a knock against a table cannot
 * set it off on its own. A gesture is two double taps within
 * DOUBLE_TAP_PAIR_WINDOW_MS of each other:
 *
 *   gesture 1                        -> buzz for CONFIRM_FEEDBACK_MS, and arm
 *   gesture 2 started within
 *   CONFIRM_WINDOW_MS of the buzz    -> notify the phone
 *
 * The confirmation window is judged against the *first* tap of the second
 * gesture, so the five seconds are the time to start repeating, not to finish.
 *
 * Everything is decided by comparing k_uptime_get() against the previous event,
 * so no step waits and the work queue is never held up. Nothing times the
 * windows out either: a late tap simply starts a fresh gesture.
 */
#define DOUBLE_TAP_PAIR_WINDOW_MS 2000
#define CONFIRM_WINDOW_MS	  5000
#define CONFIRM_FEEDBACK_MS	  1000

/* A single free-fall report is far too easy to provoke, so a drop only counts
 * once more than FALL_EVENT_COUNT of them arrive inside FALL_WINDOW_MS.
 */
#define FALL_EVENT_COUNT   4
#define FALL_WINDOW_MS	   1000
#define FALL_ALARM_ON_MS   500
#define FALL_ALARM_OFF_MS  500

static bool fall_alarm_active;

static void fall_alarm_start(void)
{
	int err;

	if (fall_alarm_active) {
		return;
	}

	fall_alarm_active = true;

	LOG_WRN("Fall detected -> alarm");

	outputs_blink_start(&buzzer, FALL_ALARM_ON_MS, FALL_ALARM_OFF_MS);
	outputs_blink_start(&vibration_motor, FALL_ALARM_ON_MS, FALL_ALARM_OFF_MS);

	err = app_gatt_notify_fall();
	if (err) {
		LOG_WRN("Failed to notify fall (err %d)", err);
	}
}

static void fall_alarm_stop(const char *reason)
{
	if (!fall_alarm_active) {
		return;
	}

	fall_alarm_active = false;

	outputs_blink_stop(&buzzer);
	outputs_blink_stop(&vibration_motor);

	LOG_INF("Fall alarm stopped (%s)", reason);
}

/* Runs on the Bluetooth thread when the peer writes 0 to the characteristic. */
static void on_fall_cleared_by_peer(void)
{
	fall_alarm_stop("peer");
}

static void handle_free_fall(void)
{
	static int64_t window_start_ms;
	static uint8_t count;

	int64_t now = k_uptime_get();

	if (count == 0 || (now - window_start_ms) > FALL_WINDOW_MS) {
		window_start_ms = now;
		count = 1;
	} else {
		count++;
	}

	LOG_INF("Free fall (%u in window)", count);

	if (count > FALL_EVENT_COUNT) {
		count = 0;
		fall_alarm_start();
	}
}

static void handle_double_tap(void)
{
	static int64_t first_tap_ms;
	static bool have_first_tap;
	static int64_t armed_ms;
	static bool awaiting_confirm;
	static bool gesture_confirms;

	int64_t now = k_uptime_get();
	int err;

	LOG_INF("Double tap");

	/* No pending first tap, or the pending one went stale: this is the first
	 * tap of a new gesture.
	 */
	if (!have_first_tap || (now - first_tap_ms) > DOUBLE_TAP_PAIR_WINDOW_MS) {
		first_tap_ms = now;
		have_first_tap = true;

		/* Decide here, not when the pair completes, so the confirmation
		 * window covers when the gesture starts.
		 */
		gesture_confirms = awaiting_confirm && (now - armed_ms) <= CONFIRM_WINDOW_MS;

		return;
	}

	/* Second tap inside the window: the gesture is complete. Clearing this
	 * stops a third tap from pairing with the one just consumed.
	 */
	have_first_tap = false;

	/* While the tag is screaming, the gesture silences it instead of asking
	 * the phone for anything - dismissing the alarm is what the user means.
	 */
	if (fall_alarm_active) {
		gesture_confirms = false;
		fall_alarm_stop("gesture");
		return;
	}

	if (gesture_confirms) {
		awaiting_confirm = false;
		gesture_confirms = false;

		LOG_INF("Gesture confirmed -> find phone");

		err = app_gatt_notify_find_phone();
		if (err) {
			LOG_WRN("Failed to notify find-phone (err %d)", err);
		}

		return;
	}

	armed_ms = now;
	awaiting_confirm = true;

	LOG_INF("Gesture armed, repeat within %d ms to confirm", CONFIRM_WINDOW_MS);

	err = outputs_pulse(&vibration_motor, CONFIRM_FEEDBACK_MS);
	if (err) {
		LOG_WRN("Failed to buzz (err %d)", err);
	}
}

/* Also runs on the system work queue. */
static void on_imu_event(enum imu_event event)
{
	switch (event) {
	case IMU_EVENT_WAKE_UP:
		LOG_INF("Motion detected");
		break;
	case IMU_EVENT_INACTIVE:
		LOG_INF("Inactive");
		break;
	case IMU_EVENT_SINGLE_TAP:
		LOG_INF("Single tap");
		break;
	case IMU_EVENT_DOUBLE_TAP:
		handle_double_tap();
		break;
	case IMU_EVENT_FREE_FALL:
		handle_free_fall();
		break;
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
	LOG_ERR("1");
	select_ceramic_antenna();
	LOG_ERR("1.5");	
	LOG_ERR("1.70");
	app_gatt_set_fall_cleared_cb(on_fall_cleared_by_peer);

	err = ble_cs_start();	
	LOG_ERR("1.75");	
	if (err) {
		return -1;
		LOG_ERR("CS init failed (err %d)", err);
	}
	LOG_ERR("2");	
	/* Last, so that a button press can never reach the Bluetooth stack
	 * before advertising is running.
	 */
	err = button_init(on_button_change);
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return -1;
	}
	LOG_ERR("3");
	/* At the +/-2 g full scale set in prj.conf: wake threshold 8 is about
	 * 250 mg, and requiring 2 consecutive samples rejects single knocks.
	 * Turn these down if the tag misses being picked up, up if it triggers
	 * on its own.
	 */
	static const struct imu_motion_config motion_cfg = {
		.wake_threshold = 32,	/* ~1 g - the tag has to really be moved */
		.wake_duration = 3,	/* max: motion must persist ~30 ms */
		.tap_threshold = 12,
		.detect_single_tap = false,
		.detect_double_tap = true,
		/* The sensor drops its own rate once nothing has moved for about
		 * 25 s, and raises it again by itself when motion returns.
		 */
		.inactivity_mode = IMU_INACTIVITY_ACCEL_LOW_RATE,
		.inactivity_duration = 5,
		/* Free fall, tuned to catch short drops: a hard throw at the floor
		 * is airborne for far less time than a gentle toss onto a table,
		 * so the duration is what was missing it. 5 is roughly 406 mg and
		 * 2 samples is roughly 20 ms.
		 */
		.freefall_threshold = 5,
		.freefall_duration = 2,
	};

	err = imu_motion_detect_enable(on_imu_event, &motion_cfg);
	if (err) {
		LOG_WRN("Motion detection unavailable (err %d)", err);
	}
	LOG_ERR("4");
	ble_cs_run();

	return 0;
}
