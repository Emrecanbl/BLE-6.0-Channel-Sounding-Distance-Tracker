/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "app_gatt.h"
#include "button.h"
#include "outputs.h"

LOG_MODULE_REGISTER(app_gatt, LOG_LEVEL_INF);

/* Built on the Nordic LED Button Service UUIDs so that nRF Connect and nRF
 * Toolbox recognise the first three characteristics out of the box; the buzzer
 * and vibration motor continue the same series.
 */
#define BT_UUID_BTN_SVC_VAL                                                                        \
	BT_UUID_128_ENCODE(0x00001523, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_BTN_CHRC_VAL                                                                       \
	BT_UUID_128_ENCODE(0x00001524, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_LED_CHRC_VAL                                                                       \
	BT_UUID_128_ENCODE(0x00001525, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_BUZZER_CHRC_VAL                                                                    \
	BT_UUID_128_ENCODE(0x00001526, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_VIB_CHRC_VAL                                                                       \
	BT_UUID_128_ENCODE(0x00001527, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_FIND_PHONE_CHRC_VAL                                                                \
	BT_UUID_128_ENCODE(0x00001528, 0x1212, 0xefde, 0x1523, 0x785feabcd123)
#define BT_UUID_FALL_CHRC_VAL                                                                      \
	BT_UUID_128_ENCODE(0x00001529, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

#define BT_UUID_BTN_SVC     BT_UUID_DECLARE_128(BT_UUID_BTN_SVC_VAL)
#define BT_UUID_BTN_CHRC    BT_UUID_DECLARE_128(BT_UUID_BTN_CHRC_VAL)
#define BT_UUID_LED_CHRC    BT_UUID_DECLARE_128(BT_UUID_LED_CHRC_VAL)
#define BT_UUID_BUZZER_CHRC BT_UUID_DECLARE_128(BT_UUID_BUZZER_CHRC_VAL)
#define BT_UUID_VIB_CHRC    BT_UUID_DECLARE_128(BT_UUID_VIB_CHRC_VAL)
#define BT_UUID_FIND_PHONE_CHRC BT_UUID_DECLARE_128(BT_UUID_FIND_PHONE_CHRC_VAL)
#define BT_UUID_FALL_CHRC	BT_UUID_DECLARE_128(BT_UUID_FALL_CHRC_VAL)

/* Backing store for the button characteristic value. Kept in sync from the
 * notify path so a plain read returns the current level too.
 */
static uint8_t button_value;
static bool button_notify_enabled;

/* Incremented every time the user asks for the phone. Readable as well as
 * notified, so a client that reconnects can see whether it missed anything.
 */
static uint8_t find_phone_count;
static bool find_phone_notify_enabled;

static ssize_t read_button(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	button_value = button_get_state();

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &button_value,
				 sizeof(button_value));
}

static void button_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	button_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Button notifications %s", button_notify_enabled ? "enabled" : "disabled");
}

static ssize_t read_find_phone(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &find_phone_count,
				 sizeof(find_phone_count));
}

/* Same shape as the find-phone counter: raised by the tag, cleared by the peer. */
static uint8_t fall_count;
static bool fall_notify_enabled;
static app_gatt_fall_cleared_cb_t fall_cleared_cb;

/* Defined below the service, which they have to reference. */
static void find_phone_notify_work_handler(struct k_work *work);
static void fall_notify_work_handler(struct k_work *work);

static K_WORK_DEFINE(find_phone_notify_work, find_phone_notify_work_handler);
static K_WORK_DEFINE(fall_notify_work, fall_notify_work_handler);

static ssize_t read_fall(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &fall_count, sizeof(fall_count));
}

static ssize_t write_fall(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(fall_count)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (((const uint8_t *)buf)[0] != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	fall_count = 0;
	LOG_INF("Fall alarm cleared by peer");

	if (fall_cleared_cb) {
		fall_cleared_cb();
	}

	k_work_submit(&fall_notify_work);

	return len;
}

static void fall_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	fall_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Fall notifications %s", fall_notify_enabled ? "enabled" : "disabled");
}

/* The phone writes 0 to say it has handled - or dismissed - the request, which
 * clears the counter. Any other value is refused, so the count can only ever be
 * raised by the tag itself.
 */
static ssize_t write_find_phone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(find_phone_count)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (((const uint8_t *)buf)[0] != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	find_phone_count = 0;
	LOG_INF("Find-phone cancelled by peer");

	/* Push the cleared value out, otherwise a client that follows the
	 * characteristic through notifications keeps showing the old count until
	 * it happens to read again. Deferred, so the write response goes out
	 * before the notification.
	 */
	k_work_submit(&find_phone_notify_work);

	return len;
}

static void find_phone_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	find_phone_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Find-phone notifications %s", find_phone_notify_enabled ? "enabled" : "disabled");
}

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

/* Each characteristic carries a User Description descriptor (0x2901) so that a
 * generic GATT client shows a name instead of just the 128-bit UUID. The button
 * value stays at attrs[2] - descriptors are appended after it, never before.
 */
BT_GATT_SERVICE_DEFINE(app_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_BTN_SVC),

	BT_GATT_CHARACTERISTIC(BT_UUID_BTN_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_button, NULL, &button_value),
	BT_GATT_CCC(button_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Button", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_LED_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &led),
	BT_GATT_CUD("LED", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_BUZZER_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &buzzer),
	BT_GATT_CUD("Buzzer", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_VIB_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_output, write_output, &vibration_motor),
	BT_GATT_CUD("Vibration motor", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_FIND_PHONE_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_find_phone, write_find_phone, &find_phone_count),
	BT_GATT_CCC(find_phone_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Find phone", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(BT_UUID_FALL_CHRC,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_fall, write_fall, &fall_count),
	BT_GATT_CCC(fall_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Fall detected", BT_GATT_PERM_READ),
);

int app_gatt_notify_button(uint8_t state)
{
	if (!button_notify_enabled) {
		return 0;
	}

	button_value = state;

	/* attrs[2] is the button characteristic value attribute. */
	return bt_gatt_notify(NULL, &app_svc.attrs[2], &button_value, sizeof(button_value));
}

/* Sends whatever the counter currently holds to a subscribed client. */
static int notify_find_phone_value(void)
{
	const struct bt_gatt_attr *attr;

	if (!find_phone_notify_enabled) {
		return 0;
	}

	/* Looked up rather than indexed, so inserting characteristics above it
	 * cannot silently point this at the wrong attribute.
	 */
	attr = bt_gatt_find_by_uuid(app_svc.attrs, app_svc.attr_count, BT_UUID_FIND_PHONE_CHRC);
	if (!attr) {
		return -ENOENT;
	}

	return bt_gatt_notify(NULL, attr, &find_phone_count, sizeof(find_phone_count));
}

static void find_phone_notify_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	err = notify_find_phone_value();
	if (err) {
		LOG_WRN("Failed to notify find-phone value (err %d)", err);
	}
}

int app_gatt_notify_find_phone(void)
{
	find_phone_count++;

	return notify_find_phone_value();
}

static int notify_fall_value(void)
{
	const struct bt_gatt_attr *attr;

	if (!fall_notify_enabled) {
		return 0;
	}

	attr = bt_gatt_find_by_uuid(app_svc.attrs, app_svc.attr_count, BT_UUID_FALL_CHRC);
	if (!attr) {
		return -ENOENT;
	}

	return bt_gatt_notify(NULL, attr, &fall_count, sizeof(fall_count));
}

static void fall_notify_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	err = notify_fall_value();
	if (err) {
		LOG_WRN("Failed to notify fall value (err %d)", err);
	}
}

void app_gatt_set_fall_cleared_cb(app_gatt_fall_cleared_cb_t cb)
{
	fall_cleared_cb = cb;
}

int app_gatt_notify_fall(void)
{
	fall_count++;

	return notify_fall_value();
}
