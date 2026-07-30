/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief LSM6DS3TR-C accelerometer and gyroscope sampling.
 */

#ifndef IMU_H_
#define IMU_H_

#include <stdbool.h>
#include <stdint.h>

/** One filtered IMU reading. */
struct imu_sample {
	double accel[3];	/**< Acceleration, m/s^2, X/Y/Z. */
	double gyro[3];		/**< Angular velocity, rad/s, X/Y/Z. */
	int64_t uptime_ms;	/**< k_uptime_get() when the sample was taken. */
};

/** Copy the most recent filtered sample.
 *
 *  This is the seam a gesture or activity classifier should read from, rather
 *  than talking to the sensor driver directly.
 *
 *  @retval 0        Sample copied.
 *  @retval -EAGAIN  No sample taken yet.
 */
int imu_get_latest(struct imu_sample *out);

/** True once the sensor has been probed successfully. */
bool imu_is_ready(void);

#endif /* IMU_H_ */
