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

/* ------------------------------------------------------------------------
 * Motion detection.
 *
 * The Zephyr LSM6DSL driver only implements SENSOR_TRIG_DATA_READY, so the
 * wake-up, tap and free-fall engines are not reachable through the sensor API.
 * They are configured here by writing the chip's registers directly over the
 * same I2C bus, and INT1 is taken as a plain GPIO interrupt.
 *
 * This only works while the driver is built with CONFIG_LSM6DSL_TRIGGER_NONE:
 * otherwise the driver claims irq-gpios for its own data-ready handler.
 *
 * Register map: LSM6DS3TR-C datasheet, register-compatible with the LSM6DSL.
 * ------------------------------------------------------------------------ */

#define LSM6_REG_WAKE_UP_SRC		 0x1B
#define LSM6_WAKE_UP_SRC_FF_IA		 BIT(5)
#define LSM6_WAKE_UP_SRC_SLEEP_STATE_IA	 BIT(4)
#define LSM6_WAKE_UP_SRC_WU_IA		 BIT(3)

#define LSM6_REG_TAP_SRC 0x1C
#define LSM6_TAP_SRC_SINGLE_TAP BIT(5)
#define LSM6_TAP_SRC_DOUBLE_TAP BIT(4)

#define LSM6_REG_TAP_CFG 0x58
#define LSM6_TAP_CFG_INTERRUPTS_ENABLE BIT(7)
#define LSM6_TAP_CFG_INACT_EN_SHIFT    5
#define LSM6_TAP_CFG_INACT_EN_MASK     0x03
#define LSM6_TAP_CFG_SLOPE_FDS	       BIT(4)
#define LSM6_TAP_CFG_TAP_X_EN	       BIT(3)
#define LSM6_TAP_CFG_TAP_Y_EN	       BIT(2)
#define LSM6_TAP_CFG_TAP_Z_EN	       BIT(1)
#define LSM6_TAP_CFG_LIR	       BIT(0)

#define LSM6_REG_TAP_THS_6D   0x59
#define LSM6_TAP_THS_6D_MASK  0x1F

/* DUR = double-tap window, QUIET = dead time after a peak, SHOCK = maximum
 * duration of the peak itself. 0x7F is the value ST's own examples use and it
 * works for a tap through a plastic enclosure.
 */
#define LSM6_REG_INT_DUR2	0x5A
#define LSM6_INT_DUR2_DEFAULT	0x7F

#define LSM6_REG_WAKE_UP_THS		 0x5B
#define LSM6_WAKE_UP_THS_MASK		 0x3F
#define LSM6_WAKE_UP_THS_SINGLE_DOUBLE_TAP BIT(7)

/* FF_DUR is split across two registers: the low five bits sit at 7:3 here, the
 * sixth is WAKE_UP_DUR bit 7. Note that the Zephyr driver header carries a
 * FREE_FALL duration shift of 4 while its own mask covers bits 7:3 - the driver
 * never touches free fall, so the inconsistency has gone unnoticed. 3 is right.
 */
#define LSM6_REG_FREE_FALL	  0x5D
#define LSM6_FREE_FALL_DUR_SHIFT  3
#define LSM6_FREE_FALL_DUR_MASK	  0x1F
#define LSM6_FREE_FALL_THS_MASK	  0x07
#define LSM6_FREE_FALL_DUR5_BIT	  BIT(5)

#define LSM6_REG_WAKE_UP_DUR	     0x5C
#define LSM6_WAKE_UP_DUR_FF_DUR5     BIT(7)
#define LSM6_WAKE_UP_DUR_WAKE_SHIFT  5
#define LSM6_WAKE_UP_DUR_WAKE_MASK   0x03
#define LSM6_WAKE_UP_DUR_SLEEP_MASK  0x0F

#define LSM6_REG_MD1_CFG 0x5E
#define LSM6_MD1_CFG_INT1_INACT_STATE BIT(7)
#define LSM6_MD1_CFG_INT1_SINGLE_TAP BIT(6)
#define LSM6_MD1_CFG_INT1_WU	     BIT(5)
#define LSM6_MD1_CFG_INT1_FF	     BIT(4)
#define LSM6_MD1_CFG_INT1_DOUBLE_TAP BIT(3)
/** One filtered IMU reading. */
struct imu_sample {
	double accel[3];	/**< Acceleration, m/s^2, X/Y/Z. */
	double gyro[3];		/**< Angular velocity, rad/s, X/Y/Z. */
	int64_t uptime_ms;	/**< k_uptime_get() when the sample was taken. */
};

/** Events the LSM6DS3TR-C interrupt engine can raise on its INT1 pin. */
enum imu_event {
	IMU_EVENT_WAKE_UP,	/**< Acceleration crossed the wake-up threshold. */
	IMU_EVENT_INACTIVE,	/**< Nothing moved for the configured period. */
	IMU_EVENT_SINGLE_TAP,
	IMU_EVENT_DOUBLE_TAP,
	IMU_EVENT_FREE_FALL,
};

/** What the sensor does to itself once it decides nothing is moving.
 *
 *  In every mode except OFF the accelerometer drops to 12.5 Hz on its own and
 *  climbs back up when motion resumes, without the MCU being involved.
 */
enum imu_inactivity_mode {
	IMU_INACTIVITY_OFF = 0,
	IMU_INACTIVITY_ACCEL_LOW_RATE = 1,	/**< Accelerometer to 12.5 Hz. */
	IMU_INACTIVITY_GYRO_SLEEP = 2,		/**< ... and gyroscope to sleep. */
	IMU_INACTIVITY_GYRO_OFF = 3,		/**< ... and gyroscope powered down. */
};

/** Motion event callback.
 *
 *  Invoked from the system work queue - never from interrupt context - because
 *  identifying the event needs an I2C read.
 */
typedef void (*imu_event_cb_t)(enum imu_event event);

/** Tuning for the sensor's on-chip detection engines. */
struct imu_motion_config {
	/** Wake-up threshold, 0-63, in units of full-scale/64. At the +/-2 g
	 *  full scale one step is about 31 mg: 2 fires on a nudge, 8 needs a
	 *  deliberate movement, 16 needs a real shake.
	 */
	uint8_t wake_threshold;
	/** Consecutive samples that must stay above the threshold, 0-3. Each
	 *  step is one accelerometer period (about 10 ms at 104 Hz). Raising
	 *  this rejects single knocks and vibration far better than raising the
	 *  threshold alone.
	 */
	uint8_t wake_duration;
	/** Tap threshold, 0-31, in units of full-scale/32 (about 62 mg per step
	 *  at +/-2 g). Around 12 suits a tap through a plastic case.
	 */
	uint8_t tap_threshold;
	/** Report single taps.
	 *
	 *  Note that a double tap always trips the single-tap detector first, so
	 *  enabling both means every double tap also produces a single-tap
	 *  event. Leave this off if only double taps are wanted.
	 */
	bool detect_single_tap;
	/** Report double taps. */
	bool detect_double_tap;
	/** Free-fall threshold, 0-7, selecting from the sensor's fixed table of
	 *  roughly 156 mg (0) to 500 mg (7). Lower means the tag has to be
	 *  closer to true weightlessness, which cuts false positives but misses
	 *  short or tumbling drops.
	 */
	uint8_t freefall_threshold;
	/** How many accelerometer periods free fall must last, 0-63. One step is
	 *  about 10 ms at 104 Hz, so 6 is roughly 60 ms. Note this scales with
	 *  the output data rate, which inactivity_mode may lower.
	 */
	uint8_t freefall_duration;
	/** What the sensor should do once it declares inactivity. */
	enum imu_inactivity_mode inactivity_mode;
	/** How long everything must stay below the wake-up threshold before
	 *  inactivity is declared, 0-15. One step is 512 accelerometer periods,
	 *  so about 4.9 s at 104 Hz; 0 means roughly 150 ms.
	 */
	uint8_t inactivity_duration;
};

/** Turn on the sensor's own detection engines and route them to INT1.
 *
 *  The accelerometer keeps running inside the sensor and only raises the pin
 *  when something happens, so the MCU does not have to poll to notice motion.
 */
int imu_motion_detect_enable(imu_event_cb_t cb, const struct imu_motion_config *cfg);

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
