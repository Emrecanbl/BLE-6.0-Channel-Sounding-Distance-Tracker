/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "outputs.h"

LOG_MODULE_REGISTER(outputs, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)

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

struct gpio_output led = {
	.spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	.name = "LED",
};

struct gpio_output buzzer = {
	.spec = { .port = DEVICE_DT_GET(BUZZER_PORT), .pin = BUZZER_PIN,
		  .dt_flags = BUZZER_FLAGS },
	.name = "Buzzer",
};

struct gpio_output vibration_motor = {
	.spec = { .port = DEVICE_DT_GET(VIB_PORT), .pin = VIB_PIN, .dt_flags = VIB_FLAGS },
	.name = "Vibration motor",
};

static struct gpio_output *const outputs[] = { &led, &buzzer, &vibration_motor };

static void output_tick_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct gpio_output *out = CONTAINER_OF(dwork, struct gpio_output, tick_work);
	uint8_t next;

	if (!out->repeating) {
		/* End of a one-shot pulse. */
		outputs_set(out, 0);
		return;
	}

	next = out->state ? 0 : 1;
	outputs_set(out, next);

	k_work_reschedule(&out->tick_work, K_MSEC(next ? out->on_ms : out->off_ms));
}

int outputs_init(void)
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

		k_work_init_delayable(&out->tick_work, output_tick_work_handler);
	}

	return 0;
}

int outputs_set(struct gpio_output *out, uint8_t state)
{
	int err;

	state = (state != 0) ? 1 : 0;

	err = gpio_pin_set_dt(&out->spec, state);
	if (err) {
		LOG_ERR("Failed to drive %s (err %d)", out->name, err);
		return err;
	}

	out->state = state;

	return 0;
}

int outputs_pulse(struct gpio_output *out, uint32_t duration_ms)
{
	int err;

	out->repeating = false;

	err = outputs_set(out, 1);
	if (err) {
		return err;
	}

	err = k_work_reschedule(&out->tick_work, K_MSEC(duration_ms));

	return (err < 0) ? err : 0;
}

int outputs_blink_start(struct gpio_output *out, uint32_t on_ms, uint32_t off_ms)
{
	int err;

	out->on_ms = on_ms;
	out->off_ms = off_ms;
	out->repeating = true;

	err = outputs_set(out, 1);
	if (err) {
		return err;
	}

	err = k_work_reschedule(&out->tick_work, K_MSEC(on_ms));

	return (err < 0) ? err : 0;
}

int outputs_blink_stop(struct gpio_output *out)
{
	out->repeating = false;

	/* Cancel before clearing the pin, so a tick already on its way cannot
	 * switch the output back on afterwards.
	 */
	k_work_cancel_delayable(&out->tick_work);

	return outputs_set(out, 0);
}
