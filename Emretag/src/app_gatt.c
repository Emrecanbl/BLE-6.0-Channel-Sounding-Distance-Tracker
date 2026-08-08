/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "app_gatt.h"
#include "ble_cs.h"
#include "button.h"
#include "imu.h"
#include "outputs.h"

LOG_MODULE_REGISTER(app_gatt, LOG_LEVEL_INF);

/* Built on the Nordic LED Button Service UUIDs, so nRF Connect recognises the
 * first characteristics without help; the rest continue the same series.
 */
#define UUID_BASE(first) BT_UUID_128_ENCODE(first, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

#define BT_UUID_APP_SVC	   BT_UUID_DECLARE_128(UUID_BASE(0x00001523))
#define BT_UUID_BUTTON	   BT_UUID_DECLARE_128(UUID_BASE(0x00001524))
#define BT_UUID_LED	   BT_UUID_DECLARE_128(UUID_BASE(0x00001525))
#define BT_UUID_BUZZER	   BT_UUID_DECLARE_128(UUID_BASE(0x00001526))
#define BT_UUID_VIB	   BT_UUID_DECLARE_128(UUID_BASE(0x00001527))
#define BT_UUID_FIND_PHONE BT_UUID_DECLARE_128(UUID_BASE(0x00001528))
#define BT_UUID_FALL	   BT_UUID_DECLARE_128(UUID_BASE(0x00001529))

/* Defined with the behaviour further down. */
static void fall_alarm_stop(const char *reason);

/* ==========================================================================
 * The profile
 * ========================================================================== */

/* An event the tag raises and the phone acknowledges by writing 0.
 *
 * A counter rather than a flag, so a phone that was disconnected or missed a
 * notification can still see that something happened.
 */
struct event_counter {
	uint8_t count;
	bool notify_enabled;
	const char *name;
};

static struct event_counter find_phone = { .name = "Find phone" };
static struct event_counter fall = { .name = "Fall" };

/* Mirrors the button pin so a plain read returns the current level. */
static uint8_t button_value;
static bool button_notify_enabled;

static int notify_value(const struct bt_uuid *uuid, const uint8_t *value)
{
	const struct bt_gatt_attr *attr;

	/* Looked up rather than indexed into app_svc.attrs, so that inserting a
	 * characteristic cannot silently move the target.
	 */
	attr = bt_gatt_find_by_uuid(app_svc.attrs, app_svc.attr_count, uuid);
	if (!attr) {
		return -ENOENT;
	}

	return bt_gatt_notify(NULL, attr, value, sizeof(*value));
}

/* --- Button --- */

static ssize_t read_button(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	button_value = button_get_state();

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &button_value,
				 sizeof(button_value));
}

static void button_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	button_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* --- Outputs: LED, buzzer, vibration motor --- */

static ssize_t read_output(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	const struct gpio_output *out = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &out->state, sizeof(out->state));
}

static ssize_t write_output(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			    uint16_t len, uint16_t offset, uint8_t flags)
{
	struct gpio_output *out = attr->user_data;

	ARG_UNUSED(conn);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(out->state)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	outputs_set(out, ((const uint8_t *)buf)[0]);

	LOG_INF("%s set to %u by peer", out->name, out->state);

	return len;
}

/* --- Event counters: find phone, fall --- */

static ssize_t read_counter(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	const struct event_counter *ev = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &ev->count, sizeof(ev->count));
}

/* The phone writes 0 to say it has handled - or dismissed - the event. Any
 * other value is refused, so the count can only ever be raised by the tag.
 */
static ssize_t write_counter(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	struct event_counter *ev = attr->user_data;

	ARG_UNUSED(conn);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(ev->count)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (((const uint8_t *)buf)[0] != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	ev->count = 0;
	LOG_INF("%s cleared by peer", ev->name);

	if (ev == &fall) {
		fall_alarm_stop("peer");
	}

	return len;
}

static void find_phone_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	find_phone.notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void fall_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	fall.notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* Every characteristic carries a User Description descriptor (0x2901), so a
 * generic client shows a name instead of a bare 128-bit UUID.
 */
BT_GATT_SERVICE_DEFINE(app_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_APP_SVC),

	BT_GATT_CHARACTERISTIC(BT_UUID_BUTTON,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_button, NULL, &button_value),
	BT_GATT_CCC(button_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Button", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_LED,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &led),
	BT_GATT_CUD("LED", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_BUZZER,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &buzzer),
	BT_GATT_CUD("Buzzer", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_VIB,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &vibration_motor),
	BT_GATT_CUD("Vibration motor", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_FIND_PHONE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_counter, write_counter, &find_phone),
	BT_GATT_CCC(find_phone_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Find phone", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_FALL,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_counter, write_counter, &fall),
	BT_GATT_CCC(fall_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Fall detected", BT_GATT_PERM_READ),
);

int app_gatt_notify_button(uint8_t state)
{
	button_value = state;

	if (!button_notify_enabled) {
		return 0;
	}

	return notify_value(BT_UUID_BUTTON, &button_value);
}

int app_gatt_notify_find_phone(void)
{
	find_phone.count++;

	if (!find_phone.notify_enabled) {
		return 0;
	}

	return notify_value(BT_UUID_FIND_PHONE, &find_phone.count);
}

int app_gatt_notify_fall(void)
{
	fall.count++;

	if (!fall.notify_enabled) {
		return 0;
	}

	return notify_value(BT_UUID_FALL, &fall.count);
}

/* ==========================================================================
 * The behaviour: what the tag does when it is tapped or dropped.
 *
 * All of it runs on the system work queue, never in interrupt context, so the
 * blocking Bluetooth calls above are safe to use from here.
 * ========================================================================== */

/* Two double taps within DOUBLE_TAP_PAIR_MS are one gesture. It takes two
 * gestures to call the phone, because a single knock against a table is far too
 * easy to produce by accident:
 *
 *   gesture 1                             -> buzz, and arm
 *   gesture 2 started within CONFIRM_MS   -> notify the phone
 *
 * The confirmation window is judged when the second gesture *starts*, so the
 * five seconds are the time to begin repeating, not to finish.
 *
 * Every decision is a comparison against k_uptime_get(), so nothing waits and
 * the work queue is never held up. No timer expires the windows either: a late
 * tap simply starts a fresh gesture.
 */
#define DOUBLE_TAP_PAIR_MS 2000
#define CONFIRM_MS	   5000
#define CONFIRM_BUZZ_MS	   1000

/* One free-fall report is far too easy to provoke, so a drop only counts once
 * more than FALL_BURST of them arrive inside FALL_BURST_MS.
 */
#define FALL_BURST	4
#define FALL_BURST_MS	1000
#define ALARM_ON_MS	500
#define ALARM_OFF_MS	500

static const struct imu_motion_config motion_cfg = {
	.wake_threshold = 32,	/* ~1 g - the tag has to really be moved */
	.wake_duration = 3,	/* max: motion must persist ~30 ms */
	.tap_threshold = 12,
	.detect_single_tap = false,
	.detect_double_tap = true,
	/* Tuned for short drops: a hard throw at the floor is airborne for far
	 * less time than a gentle toss onto a table.
	 */
	.freefall_threshold = 5,
	.freefall_duration = 2,
	/* The sensor drops its own sample rate after ~25 s of stillness and
	 * raises it again by itself when motion returns.
	 */
	.inactivity_mode = IMU_INACTIVITY_ACCEL_LOW_RATE,
	.inactivity_duration = 5,
};

static bool alarm_active;

static void on_button_change(uint8_t state, uint16_t press_count)
{
	app_gatt_notify_button(state);
	ble_cs_set_press_count(press_count);
}

static void fall_alarm_start(void)
{
	if (alarm_active) {
		return;
	}

	alarm_active = true;

	LOG_WRN("Fall detected -> alarm");

	outputs_blink_start(&buzzer, ALARM_ON_MS, ALARM_OFF_MS);
	outputs_blink_start(&vibration_motor, ALARM_ON_MS, ALARM_OFF_MS);

	app_gatt_notify_fall();
}

static void fall_alarm_stop(const char *reason)
{
	if (!alarm_active) {
		return;
	}

	alarm_active = false;

	outputs_blink_stop(&buzzer);
	outputs_blink_stop(&vibration_motor);

	LOG_INF("Fall alarm stopped (%s)", reason);
}

static void handle_free_fall(void)
{
	static int64_t burst_start_ms;
	static uint8_t count;

	int64_t now = k_uptime_get();

	if (count == 0 || (now - burst_start_ms) > FALL_BURST_MS) {
		burst_start_ms = now;
		count = 1;
	} else {
		count++;
	}

	LOG_INF("Free fall (%u in burst)", count);

	if (count > FALL_BURST) {
		count = 0;
		fall_alarm_start();
	}
}

static void handle_double_tap(void)
{
	static int64_t first_tap_ms;
	static bool have_first_tap;
	static int64_t armed_ms;
	static bool armed;
	static bool this_gesture_confirms;

	int64_t now = k_uptime_get();

	LOG_INF("Double tap");

	/* Nothing pending, or the pending tap went stale: this is the first tap
	 * of a new gesture.
	 */
	if (!have_first_tap || (now - first_tap_ms) > DOUBLE_TAP_PAIR_MS) {
		first_tap_ms = now;
		have_first_tap = true;
		this_gesture_confirms = armed && (now - armed_ms) <= CONFIRM_MS;
		return;
	}

	/* Second tap in time: the gesture is complete. Clearing this stops a
	 * third tap from pairing with the one just used.
	 */
	have_first_tap = false;

	/* While the tag is screaming, the gesture silences it rather than asking
	 * the phone for anything - dismissing the alarm is what the user means.
	 */
	if (alarm_active) {
		fall_alarm_stop("gesture");
		return;
	}

	if (this_gesture_confirms) {
		armed = false;
		LOG_INF("Gesture confirmed -> find phone");
		app_gatt_notify_find_phone();
		return;
	}

	armed = true;
	armed_ms = now;

	LOG_INF("Gesture armed, repeat within %d ms to confirm", CONFIRM_MS);
	outputs_pulse(&vibration_motor, CONFIRM_BUZZ_MS);
}

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

int app_gatt_init(void)
{
	int err;

	err = button_init(on_button_change);
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return err;
	}

	err = imu_motion_detect_enable(on_imu_event, &motion_cfg);
	if (err) {
		/* The tag still works as a ranging reflector without gestures. */
		LOG_WRN("Motion detection unavailable (err %d)", err);
	}

	return 0;
}
