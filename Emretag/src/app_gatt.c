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

#define BT_UUID_BTN_SVC     BT_UUID_DECLARE_128(BT_UUID_BTN_SVC_VAL)
#define BT_UUID_BTN_CHRC    BT_UUID_DECLARE_128(BT_UUID_BTN_CHRC_VAL)
#define BT_UUID_LED_CHRC    BT_UUID_DECLARE_128(BT_UUID_LED_CHRC_VAL)
#define BT_UUID_BUZZER_CHRC BT_UUID_DECLARE_128(BT_UUID_BUZZER_CHRC_VAL)
#define BT_UUID_VIB_CHRC    BT_UUID_DECLARE_128(BT_UUID_VIB_CHRC_VAL)

/* Backing store for the button characteristic value. Kept in sync from the
 * notify path so a plain read returns the current level too.
 */
static uint8_t button_value;
static bool button_notify_enabled;

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
