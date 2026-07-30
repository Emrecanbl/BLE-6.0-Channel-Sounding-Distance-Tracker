/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
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

#if IMU_LOG_SAMPLES
		LOG_INF("accel %7.3f %7.3f %7.3f m/s2   gyro %7.3f %7.3f %7.3f rad/s",
			latest.accel[0], latest.accel[1], latest.accel[2],
			latest.gyro[0], latest.gyro[1], latest.gyro[2]);
#endif

		k_msleep(IMU_SAMPLE_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(imu_thread, IMU_STACK_SIZE, imu_thread_fn, NULL, NULL, NULL, IMU_PRIORITY, 0, 0);
