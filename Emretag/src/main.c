/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Channel Sounding Reflector with Ranging Responder sample
 */

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/cs.h>
#include <bluetooth/services/ras.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/drivers/regulator.h> // Antenna
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

#define COMPANY_ID_CODE 0x0059

#define SW0_NODE  DT_ALIAS(sw0)
#define LED0_NODE DT_ALIAS(led0)
#define IMU_NODE  DT_ALIAS(imu0)

/* Buzzer and vibration motor are not described by the board devicetree, so the
 * pins are given here. Port/pin follow the XIAO connector mapping in
 * seeed_xiao_connector.dtsi; use GPIO_ACTIVE_LOW if the driver transistor turns
 * the load on when the pin is pulled low.
 *
 * CHANGE THESE TO MATCH YOUR WIRING - D0/D1 are only placeholders. Watch out
 * for gpio2: it also carries the LED (pin 0) and the RF switch (pins 3 and 5).
 */
#define BUZZER_PORT  DT_NODELABEL(gpio1)
#define BUZZER_PIN   4			/* D0 -> P1.04 */
#define BUZZER_FLAGS GPIO_ACTIVE_HIGH

#define VIB_PORT  DT_NODELABEL(gpio1)
#define VIB_PIN   5			/* D1 -> P1.05 */
#define VIB_FLAGS GPIO_ACTIVE_HIGH

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* A GPIO output the peer can switch on and off over GATT. */
struct gpio_output {
	const struct gpio_dt_spec spec;
	const char *name;
	uint8_t state;
};

/* The LED doubles as the connection status indicator. */
static struct gpio_output led = {
	.spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	.name = "LED",
};

static struct gpio_output buzzer = {
	.spec = { .port = DEVICE_DT_GET(BUZZER_PORT), .pin = BUZZER_PIN,
		  .dt_flags = BUZZER_FLAGS },
	.name = "Buzzer",
};

static struct gpio_output vibration_motor = {
	.spec = { .port = DEVICE_DT_GET(VIB_PORT), .pin = VIB_PIN, .dt_flags = VIB_FLAGS },
	.name = "Vibration motor",
};

static struct gpio_output *const outputs[] = { &led, &buzzer, &vibration_motor };

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_config, 0, 1);

static struct bt_conn *connection;

static const struct device *rfsw_pwr = DEVICE_DT_GET(DT_NODELABEL(rfsw_pwr)); // Antenna Enable
static const struct device *rfsw_ctl = DEVICE_DT_GET(DT_NODELABEL(rfsw_ctl));

typedef struct adv_mfg_data {
	uint16_t company_code; /* Company Identifier Code. */
	uint16_t number_press; /* Number of times Button 1 is pressed */
} adv_mfg_data_type;

static adv_mfg_data_type adv_mfg_data = { COMPANY_ID_CODE, 0x00 };

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, (unsigned char *)&adv_mfg_data, sizeof(adv_mfg_data)),
};

static const unsigned char url_data[] = {0x17, '/','/','g','i','t','h','u','b','.','c','o',
                                         'm','/','E','m','r','e','c','a','n','b','l'};

static const struct bt_data sd[] = {
        /* 4.2.3 Include the URL data in the scan response packet*/
	BT_DATA(BT_DATA_URI, url_data, sizeof(url_data)),
};

static const struct bt_le_adv_param *adv_param =
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_NONE, /* No options specified */
			800, /* Min Advertising Interval 500ms (800*0.625ms) */
			801, /* Max Advertising Interval 500.625ms (801*0.625ms) */
			NULL); /* Set to NULL for undirected advertising */

/* Button service. Uses the Nordic LED Button Service UUIDs so that nRF Connect
 * and the nRF Toolbox apps recognise the characteristic out of the box.
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

/* 1 while the button is held down, 0 once it is released. */
static uint8_t button_state;
static bool button_notify_enabled;

static ssize_t read_button(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	const uint8_t *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
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

	out->state = (((const uint8_t *)buf)[0] != 0) ? 1 : 0;
	gpio_pin_set_dt(&out->spec, out->state);

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
			       read_button, NULL, &button_state),
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

/* bt_gatt_notify() and bt_le_adv_update_data() can block, so neither may run in
 * the GPIO ISR. The ISR only samples the pin and hands off to the workqueue.
 */
static void button_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (button_notify_enabled) {
		/* attrs[2] is the characteristic value attribute. */
		err = bt_gatt_notify(NULL, &app_svc.attrs[2], &button_state,
				     sizeof(button_state));
		if (err) {
			LOG_WRN("Failed to notify button state (err %d)", err);
		}
	}

	err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_WRN("Failed to update advertising data (err %d)", err);
	}
}

static K_WORK_DEFINE(button_work, button_work_handler);

static void button_pressed(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Logical level: 1 when pressed, regardless of the active-low wiring. */
	button_state = (gpio_pin_get_dt(&button) > 0) ? 1 : 0;

	if (button_state) {
		adv_mfg_data.number_press += 1;
	}

	k_work_submit(&button_work);
}

static int init_outputs(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
		struct gpio_output *out = outputs[i];
		int err;

		if (!gpio_is_ready_dt(&out->spec)) {
			LOG_ERR("%s GPIO port is not ready", out->name);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&out->spec, GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("Failed to configure %s pin (err %d)", out->name, err);
			return err;
		}
	}

	return 0;
}

/* IMU sampling. The sensor is polled rather than driven from its INT line, so
 * this runs in its own thread and does not disturb the Bluetooth work queues.
 */
#define IMU_SAMPLE_INTERVAL_MS 500
#define IMU_STACK_SIZE	       2048
#define IMU_PRIORITY	       7

static const struct device *const imu = DEVICE_DT_GET(IMU_NODE);

/* The IMU supply hangs off P0.01 via the pdm_imu_pwr regulator. The board DTS
 * marks it regulator-boot-on, but the same is true of the RF switch nodes and
 * those still needed an explicit enable, so do not rely on it here either.
 *
 * This has to run before the sensor driver probes the chip over I2C, otherwise
 * the WHO_AM_I read fails and the device stays permanently un-ready - enabling
 * the rail from main() would be far too late. POST_KERNEL 60 sits after I2C
 * (50) and before the sensors (CONFIG_SENSOR_INIT_PRIORITY, 90).
 */
static const struct device *const imu_pwr = DEVICE_DT_GET(DT_NODELABEL(pdm_imu_pwr));

static int imu_power_on(void)
{
	int err;

	if (!device_is_ready(imu_pwr)) {
		LOG_ERR("IMU regulator is not ready");
		return -ENODEV;
	}

	err = regulator_enable(imu_pwr);
	if (err) {
		LOG_ERR("Failed to enable the IMU regulator (err %d)", err);
		return err;
	}

	/* regulator_enable() already honours the 5 ms startup-delay-us from the
	 * DTS; this covers the sensor's own power-on boot time on top of it.
	 */
	k_msleep(20);

	return 0;
}

SYS_INIT(imu_power_on, POST_KERNEL, 60);

static void imu_thread_fn(void *p1, void *p2, void *p3)
{
	struct sensor_value accel[3];
	struct sensor_value gyro[3];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(imu)) {
		LOG_ERR("IMU %s is not ready", imu->name);
		return;
	}

	LOG_INF("IMU %s ready, sampling every %d ms", imu->name, IMU_SAMPLE_INTERVAL_MS);

	while (true) {
		int err = sensor_sample_fetch(imu);

		if (err) {
			LOG_ERR("IMU sample fetch failed (err %d)", err);
			k_msleep(IMU_SAMPLE_INTERVAL_MS);
			continue;
		}

		err = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel);
		if (err) {
			LOG_ERR("Failed to read acceleration (err %d)", err);
			k_msleep(IMU_SAMPLE_INTERVAL_MS);
			continue;
		}

		err = sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyro);
		if (err) {
			LOG_ERR("Failed to read angular velocity (err %d)", err);
			k_msleep(IMU_SAMPLE_INTERVAL_MS);
			continue;
		}

		LOG_INF("accel %7.3f %7.3f %7.3f m/s2   gyro %7.3f %7.3f %7.3f rad/s",
			sensor_value_to_double(&accel[0]), sensor_value_to_double(&accel[1]),
			sensor_value_to_double(&accel[2]), sensor_value_to_double(&gyro[0]),
			sensor_value_to_double(&gyro[1]), sensor_value_to_double(&gyro[2]));

		k_msleep(IMU_SAMPLE_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(imu_thread, IMU_STACK_SIZE, imu_thread_fn, NULL, NULL, NULL, IMU_PRIORITY, 0, 0);

static int init_button(void)
{
	int err;

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button GPIO port is not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure button pin (err %d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (err) {
		LOG_ERR("Failed to configure button interrupt (err %d)", err);
		return err;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));

	err = gpio_add_callback_dt(&button, &button_cb_data);
	if (err) {
		LOG_ERR("Failed to add button callback (err %d)", err);
		return err;
	}

	return 0;
}
static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected to %s (err 0x%02X)", addr, err);

	if (err) {
		bt_conn_unref(conn);
		connection = NULL;
	} else {
		connection = bt_conn_ref(conn);

		k_sem_give(&sem_connected);

		led.state = 1;
		gpio_pin_set_dt(&led.spec, 1);
	}
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason 0x%02X)", reason);

	bt_conn_unref(conn);
	connection = NULL;

	led.state = 0;
	gpio_pin_set_dt(&led.spec, 0);

	sys_reboot(SYS_REBOOT_COLD);
}

static void remote_capabilities_cb(struct bt_conn *conn,
				   uint8_t status,
				   struct bt_conn_le_cs_capabilities *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS capability exchange completed.");
	} else {
		LOG_WRN("CS capability exchange failed. (HCI status 0x%02x)", status);
	}
}

static void config_create_cb(struct bt_conn *conn, uint8_t status,
			     struct bt_conn_le_cs_config *config)
{
	ARG_UNUSED(conn);

	if (status == BT_HCI_ERR_SUCCESS) {
		const char *mode_str[5] = {"Unused", "1 (RTT)", "2 (PBR)", "3 (RTT + PBR)",
					   "Invalid"};
		const char *role_str[3] = {"Initiator", "Reflector", "Invalid"};
		const char *rtt_type_str[8] = {
			"AA only",	 "32-bit sounding", "96-bit sounding", "32-bit random",
			"64-bit random", "96-bit random",   "128-bit random",  "Invalid"};
		const char *phy_str[4] = {"Invalid", "LE 1M PHY", "LE 2M PHY", "LE 2M 2BT PHY"};
		const char *chsel_type_str[3] = {"Algorithm #3b", "Algorithm #3c", "Invalid"};
		const char *ch3c_shape_str[3] = {"Hat shape", "X shape", "Invalid"};

		uint8_t mode_idx = config->mode > 0 && config->mode < 4 ? config->mode : 4;
		uint8_t role_idx = MIN(config->role, 2);
		uint8_t rtt_type_idx = MIN(config->rtt_type, 7);
		uint8_t phy_idx = config->cs_sync_phy > 0 && config->cs_sync_phy < 4
					  ? config->cs_sync_phy
					  : 0;
		uint8_t chsel_type_idx = MIN(config->channel_selection_type, 2);
		uint8_t ch3c_shape_idx = MIN(config->ch3c_shape, 2);

		LOG_INF("CS config creation complete.\n"
			" - id: %u\n"
			" - mode: %s\n"
			" - min_main_mode_steps: %u\n"
			" - max_main_mode_steps: %u\n"
			" - main_mode_repetition: %u\n"
			" - mode_0_steps: %u\n"
			" - role: %s\n"
			" - rtt_type: %s\n"
			" - cs_sync_phy: %s\n"
			" - channel_map_repetition: %u\n"
			" - channel_selection_type: %s\n"
			" - ch3c_shape: %s\n"
			" - ch3c_jump: %u\n"
			" - t_ip1_time_us: %u\n"
			" - t_ip2_time_us: %u\n"
			" - t_fcs_time_us: %u\n"
			" - t_pm_time_us: %u\n"
			" - channel_map: 0x%08X%08X%04X\n",
			config->id, mode_str[mode_idx],
			config->min_main_mode_steps, config->max_main_mode_steps,
			config->main_mode_repetition, config->mode_0_steps, role_str[role_idx],
			rtt_type_str[rtt_type_idx], phy_str[phy_idx],
			config->channel_map_repetition, chsel_type_str[chsel_type_idx],
			ch3c_shape_str[ch3c_shape_idx], config->ch3c_jump, config->t_ip1_time_us,
			config->t_ip2_time_us, config->t_fcs_time_us, config->t_pm_time_us,
			sys_get_le32(&config->channel_map[6]),
			sys_get_le32(&config->channel_map[2]),
			sys_get_le16(&config->channel_map[0]));

		k_sem_give(&sem_config);
	} else {
		LOG_WRN("CS config creation failed. (HCI status 0x%02x)", status);
	}
}

static void security_enable_cb(struct bt_conn *conn, uint8_t status)
{
	ARG_UNUSED(conn);

	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS security enabled.");
	} else {
		LOG_WRN("CS security enable failed. (HCI status 0x%02x)", status);
	}
}

static void procedure_enable_cb(struct bt_conn *conn,
				uint8_t status,
				struct bt_conn_le_cs_procedure_enable_complete *params)
{
	ARG_UNUSED(conn);

	if (status == BT_HCI_ERR_SUCCESS) {
		if (params->state == 1) {
			LOG_INF("CS procedures enabled:\n"
				" - config ID: %u\n"
				" - antenna configuration index: %u\n"
				" - TX power: %d dbm\n"
				" - subevent length: %u us\n"
				" - subevents per event: %u\n"
				" - subevent interval: %u\n"
				" - event interval: %u\n"
				" - procedure interval: %u\n"
				" - procedure count: %u\n"
				" - maximum procedure length: %u",
				params->config_id, params->tone_antenna_config_selection,
				params->selected_tx_power, params->subevent_len,
				params->subevents_per_event, params->subevent_interval,
				params->event_interval, params->procedure_interval,
				params->procedure_count, params->max_procedure_len);
		} else {
			LOG_INF("CS procedures disabled.");
		}
	} else {
		LOG_WRN("CS procedures enable failed. (HCI status 0x%02x)", status);
	}
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.le_cs_read_remote_capabilities_complete = remote_capabilities_cb,
	.le_cs_config_complete = config_create_cb,
	.le_cs_security_enable_complete = security_enable_cb,
	.le_cs_procedure_enable_complete = procedure_enable_cb,
};
	/*
 * XIAO nRF54L15 RF path selection.
 *
 * The board routes the radio through an RF switch that selects between the
 * onboard ceramic antenna and the external u.FL connector. Unlike the IMU
 * supply, the switch nodes in the board DTS carry no "regulator-boot-on",
 * so nothing enables them at startup. Upstream samples (including the
 * Channel Sounding ones) are unaware of this switch, which leaves the RF
 * path undefined and cripples the link budget.
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

int main(void)
{
	int err;

	LOG_INF("Starting Channel Sounding Reflector Sample");

	err = init_outputs();
	LOG_ERR("-2");
	if (err) {
		LOG_ERR("Output init failed (err %d)", err);
		return -1;
	}
	LOG_ERR("0");
	regulator_enable(rfsw_pwr);
	k_msleep(1);                  /* let the switch supply settle */
	regulator_enable(rfsw_ctl);  /* select onboard ceramic antenna */
	LOG_ERR("0");
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return -1;
	}
	LOG_ERR("1");
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}
	LOG_ERR("2");
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return -1;
	}
	LOG_ERR("3");
	/* Set up after advertising is running, so a button press can never reach
	 * bt_le_adv_update_data() before the stack is ready.
	 */
	err = init_button();
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return -1;
	}
	LOG_ERR("4");
	while (true) {
		k_sem_take(&sem_connected, K_FOREVER);

		const struct bt_le_cs_set_default_settings_param default_settings = {
			.enable_initiator_role = false,
			.enable_reflector_role = true,
			.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
			.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
		};
		LOG_ERR("5(err %d)", err);
		err = bt_le_cs_set_default_settings(connection, &default_settings);
		if (err) {
			LOG_ERR("Failed to configure default CS settings (err %d)", err);
		}
		LOG_ERR("5");
		k_sem_take(&sem_config, K_FOREVER);

		const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
			.config_id = 0,
			.max_procedure_len = 1000,
			.min_procedure_interval = 1,
			.max_procedure_interval = 100,
			.max_procedure_count = 0,
			.min_subevent_len = 10000,
			.max_subevent_len = 75000,
			.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
			.phy = BT_LE_CS_PROCEDURE_PHY_2M,
			.tx_power_delta = 0x80,
			.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
			.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
			.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
		};

		err = bt_le_cs_set_procedure_parameters(connection, &procedure_params);
		if (err) {
			LOG_ERR("Failed to set procedure parameters (err %d)", err);
			return 0;
		}

	}

	return 0;
}
