/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief LED, buzzer and vibration motor outputs.
 */

#ifndef OUTPUTS_H_
#define OUTPUTS_H_

#include <stdint.h>
#include <zephyr/drivers/gpio.h>

/** A GPIO output that can be switched on and off, locally or over GATT. */
struct gpio_output {
	const struct gpio_dt_spec spec;
	const char *name;
	uint8_t state;
};

/** Connection status indicator, also writable by the peer. */
extern struct gpio_output led;
extern struct gpio_output buzzer;
extern struct gpio_output vibration_motor;

/** Configure every output as an inactive (off) GPIO output. */
int outputs_init(void);

/** Drive @p out to @p state (0 = off, non-zero = on) and record it. */
int outputs_set(struct gpio_output *out, uint8_t state);

#endif /* OUTPUTS_H_ */
