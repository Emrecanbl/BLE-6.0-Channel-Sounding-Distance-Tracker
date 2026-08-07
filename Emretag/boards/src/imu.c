/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "imu.h"

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_NODE DT_ALIAS(imu0)

#define IMU_SAMPLE_INTERVAL_MS 500
#define IMU_STACK_SIZE	       2048
#define IMU_PRIORITY	       7

/* Exponential moving average weight applied to each new reading. 1.0 disables
 * filtering; smaller values smooth harder at the cost of response time.
 */
#define IMU_FILTER_ALPHA 0.3

/* Log every sample. Turn this off once something else consumes the data. */
#define IMU_LOG_SAMPLES 1

static const struct device *const imu = DEVICE_DT_GET(IMU_NODE);

/* The IMU and the PDM microphone share a supply rail switched by P0.01 through
 * the pdm_imu_pwr regulator. The board DTS marks it regulator-boot-on, but the
 * same is true of the RF switch nodes and those still needed an explicit
 * enable, so do not rely on it here either.
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

static struct k_mutex sample_lock;
static struct imu_sample latest;
static bool have_sample;
static bool sensor_ready;

static void filter_axis(double *state, double raw, bool first)
{
	*state = first ? raw : (IMU_FILTER_ALPHA * raw) + ((1.0 - IMU_FILTER_ALPHA) * *state);
}

static void store_sample(const struct sensor_value accel[3], const struct sensor_value gyro[3])
{
	bool first;

	k_mutex_lock(&sample_lock, K_FOREVER);

	first = !have_sample;

	for (int i = 0; i < 3; i++) {
		filter_axis(&latest.accel[i], sensor_value_to_double(&accel[i]), first);
		filter_axis(&latest.gyro[i], sensor_value_to_double(&gyro[i]), first);
	}

	latest.uptime_ms = k_uptime_get();
	have_sample = true;

	k_mutex_unlock(&sample_lock);
}

int imu_get_latest(struct imu_sample *out)
{
	int err = 0;

	k_mutex_lock(&sample_lock, K_FOREVER);

	if (have_sample) {
		*out = latest;
	} else {
		err = -EAGAIN;
	}

	k_mutex_unlock(&sample_lock);

	return err;
}

bool imu_is_ready(void)
{
	return sensor_ready;
}

/* The sensor is polled rather than driven from its INT line, so this runs in
 * its own thread and does not disturb the Bluetooth work queues.
 */
static void imu_thread_fn(void *p1, void *p2, void *p3)
{
	struct sensor_value accel[3];
	struct sensor_value gyro[3];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_mutex_init(&sample_lock);

	if (!device_is_ready(imu)) {
		LOG_ERR("IMU %s is not ready", imu->name);
		return;
	}

	sensor_ready = true;
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

		store_sample(accel, gyro);
/*
#if IMU_LOG_SAMPLES
		LOG_INF("accel %7.3f %7.3f %7.3f m/s2   gyro %7.3f %7.3f %7.3f rad/s",
			latest.accel[0], latest.accel[1], latest.accel[2],
			latest.gyro[0], latest.gyro[1], latest.gyro[2]);
#endif
*/
		k_msleep(IMU_SAMPLE_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(imu_thread, IMU_STACK_SIZE, imu_thread_fn, NULL, NULL, NULL, IMU_PRIORITY, 0, 0);


static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(IMU_NODE);
static const struct gpio_dt_spec imu_int = GPIO_DT_SPEC_GET(IMU_NODE, irq_gpios);
static struct gpio_callback imu_int_cb_data;

static imu_event_cb_t event_cb;
static struct imu_motion_config motion_cfg;

static void report(enum imu_event event)
{
	if (event_cb) {
		event_cb(event);
	}
}

/* Reading the source registers needs I2C, which cannot run in the ISR. Reading
 * them is also what clears the latched interrupt (TAP_CFG.LIR), so this must
 * run for the pin to be released.
 */
static void imu_int_work_handler(struct k_work *work)
{
	uint8_t wake_src;
	uint8_t tap_src;
	int err;

	ARG_UNUSED(work);

	err = i2c_reg_read_byte_dt(&imu_i2c, LSM6_REG_WAKE_UP_SRC, &wake_src);
	if (err) {
		LOG_ERR("Failed to read WAKE_UP_SRC (err %d)", err);
		return;
	}

	err = i2c_reg_read_byte_dt(&imu_i2c, LSM6_REG_TAP_SRC, &tap_src);
	if (err) {
		LOG_ERR("Failed to read TAP_SRC (err %d)", err);
		return;
	}

	if (wake_src & LSM6_WAKE_UP_SRC_FF_IA) {
		report(IMU_EVENT_FREE_FALL);
	}

	if (motion_cfg.inactivity_mode != IMU_INACTIVITY_OFF &&
	    (wake_src & LSM6_WAKE_UP_SRC_SLEEP_STATE_IA)) {
		report(IMU_EVENT_INACTIVE);
	}

	/* A double tap sets the single-tap bit too, so check double first and
	 * only fall through when single taps are actually wanted.
	 */
	if (motion_cfg.detect_double_tap && (tap_src & LSM6_TAP_SRC_DOUBLE_TAP)) {
		report(IMU_EVENT_DOUBLE_TAP);
	} else if (motion_cfg.detect_single_tap && (tap_src & LSM6_TAP_SRC_SINGLE_TAP)) {
		report(IMU_EVENT_SINGLE_TAP);
	}

	if (wake_src & LSM6_WAKE_UP_SRC_WU_IA) {
		report(IMU_EVENT_WAKE_UP);
	}
}

static K_WORK_DEFINE(imu_int_work, imu_int_work_handler);

static void imu_int_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&imu_int_work);
}

int imu_motion_detect_enable(imu_event_cb_t cb, const struct imu_motion_config *cfg)
{
	uint8_t tap_cfg;
	uint8_t wake_ths;
	uint8_t wake_dur;
	uint8_t md1;
	int err;

	event_cb = cb;
	motion_cfg = *cfg;

	if (!device_is_ready(imu_i2c.bus)) {
		LOG_ERR("IMU I2C bus is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&imu_int)) {
		LOG_ERR("IMU interrupt GPIO port is not ready");
		return -ENODEV;
	}

	/* Enable the interrupt engines and the tap detector on all three axes,
	 * high-pass the accelerometer data so gravity does not count towards the
	 * threshold, and latch the interrupt until the source register is read.
	 */
	tap_cfg = LSM6_TAP_CFG_INTERRUPTS_ENABLE | LSM6_TAP_CFG_SLOPE_FDS |
		  LSM6_TAP_CFG_TAP_X_EN | LSM6_TAP_CFG_TAP_Y_EN | LSM6_TAP_CFG_TAP_Z_EN |
		  LSM6_TAP_CFG_LIR;
	tap_cfg |= (cfg->inactivity_mode & LSM6_TAP_CFG_INACT_EN_MASK)
		   << LSM6_TAP_CFG_INACT_EN_SHIFT;

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_TAP_CFG, tap_cfg);
	if (err) {
		LOG_ERR("Failed to write TAP_CFG (err %d)", err);
		return err;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_TAP_THS_6D,
				    cfg->tap_threshold & LSM6_TAP_THS_6D_MASK);
	if (err) {
		LOG_ERR("Failed to write TAP_THS_6D (err %d)", err);
		return err;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_INT_DUR2, LSM6_INT_DUR2_DEFAULT);
	if (err) {
		LOG_ERR("Failed to write INT_DUR2 (err %d)", err);
		return err;
	}

	/* The double-tap detector only runs while SINGLE_DOUBLE_TAP is set; with
	 * it cleared the chip reports single taps only. Single taps are silenced
	 * by not routing them to INT1 below, not by clearing this bit.
	 */
	wake_ths = cfg->wake_threshold & LSM6_WAKE_UP_THS_MASK;
	if (cfg->detect_double_tap) {
		wake_ths |= LSM6_WAKE_UP_THS_SINGLE_DOUBLE_TAP;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_WAKE_UP_THS, wake_ths);
	if (err) {
		LOG_ERR("Failed to write WAKE_UP_THS (err %d)", err);
		return err;
	}

	wake_dur = ((cfg->wake_duration & LSM6_WAKE_UP_DUR_WAKE_MASK)
		    << LSM6_WAKE_UP_DUR_WAKE_SHIFT) |
		   (cfg->inactivity_duration & LSM6_WAKE_UP_DUR_SLEEP_MASK);

	/* The sixth bit of the free-fall duration lives here, not in FREE_FALL. */
	if (cfg->freefall_duration & LSM6_FREE_FALL_DUR5_BIT) {
		wake_dur |= LSM6_WAKE_UP_DUR_FF_DUR5;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_WAKE_UP_DUR, wake_dur);
	if (err) {
		LOG_ERR("Failed to write WAKE_UP_DUR (err %d)", err);
		return err;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_FREE_FALL,
				    ((cfg->freefall_duration & LSM6_FREE_FALL_DUR_MASK)
				     << LSM6_FREE_FALL_DUR_SHIFT) |
					    (cfg->freefall_threshold & LSM6_FREE_FALL_THS_MASK));
	if (err) {
		LOG_ERR("Failed to write FREE_FALL (err %d)", err);
		return err;
	}

	md1 = LSM6_MD1_CFG_INT1_WU | LSM6_MD1_CFG_INT1_FF;
	if (cfg->inactivity_mode != IMU_INACTIVITY_OFF) {
		md1 |= LSM6_MD1_CFG_INT1_INACT_STATE;
	}
	if (cfg->detect_single_tap) {
		md1 |= LSM6_MD1_CFG_INT1_SINGLE_TAP;
	}
	if (cfg->detect_double_tap) {
		md1 |= LSM6_MD1_CFG_INT1_DOUBLE_TAP;
	}

	err = i2c_reg_write_byte_dt(&imu_i2c, LSM6_REG_MD1_CFG, md1);
	if (err) {
		LOG_ERR("Failed to route interrupts to INT1 (err %d)", err);
		return err;
	}

	err = gpio_pin_configure_dt(&imu_int, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure IMU interrupt pin (err %d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&imu_int, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to configure IMU interrupt (err %d)", err);
		return err;
	}

	gpio_init_callback(&imu_int_cb_data, imu_int_isr, BIT(imu_int.pin));

	err = gpio_add_callback_dt(&imu_int, &imu_int_cb_data);
	if (err) {
		LOG_ERR("Failed to add IMU interrupt callback (err %d)", err);
		return err;
	}

	LOG_INF("Motion detection enabled (wake ths %u dur %u; tap ths %u, single %s double %s)",
		cfg->wake_threshold, cfg->wake_duration, cfg->tap_threshold,
		cfg->detect_single_tap ? "on" : "off",
		cfg->detect_double_tap ? "on" : "off");

	return 0;
}
