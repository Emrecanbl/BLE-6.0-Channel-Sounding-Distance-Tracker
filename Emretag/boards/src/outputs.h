/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief LED, buzzer and vibration motor outputs.
 */

#ifndef OUTPUTS_H_
#define OUTPUTS_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/** A GPIO output that can be switched on and off, locally or over GATT. */
struct gpio_output {
	const struct gpio_dt_spec spec;
	const char *name;
	uint8_t state;
	/** Drives the timed part of outputs_pulse() and outputs_blink_start(). */
	struct k_work_delayable tick_work;
	uint32_t on_ms;
	uint32_t off_ms;
	bool repeating;
};

/** Connection status indicator, also writable by the peer. */
extern struct gpio_output led;
extern struct gpio_output buzzer;
extern struct gpio_output vibration_motor;

/** Configure every output as an inactive (off) GPIO output. */
int outputs_init(void);

/** Drive @p out to @p state (0 = off, non-zero = on) and record it. */
int outputs_set(struct gpio_output *out, uint8_t state);

/** Switch @p out on and have it turn itself off again @p duration_ms later.
 *
 *  Returns immediately; the shutdown runs from the work queue. Calling it again
 *  while a pulse is running restarts the timer rather than cutting it short.
 */
int outputs_pulse(struct gpio_output *out, uint32_t duration_ms);

/** Start switching @p out on for @p on_ms and off for @p off_ms, repeatedly.
 *
 *  Returns immediately; the toggling runs from the work queue. Keeps going
 *  until outputs_blink_stop() or outputs_set().
 */
int outputs_blink_start(struct gpio_output *out, uint32_t on_ms, uint32_t off_ms);

/** Stop a running blink and leave the output off. */
int outputs_blink_stop(struct gpio_output *out);

#endif /* OUTPUTS_H_ */
