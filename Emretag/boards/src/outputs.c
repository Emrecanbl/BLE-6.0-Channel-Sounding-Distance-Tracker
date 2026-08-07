/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "outputs.h"

LOG_MODULE_REGISTER(outputs, LOG_LEVEL_INF);

/* led0 comes from the board; buzzer and vibration-motor are added by
 * boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay.
 */
#define LED0_NODE   DT_ALIAS(led0)
#define BUZZER_NODE DT_ALIAS(buzzer)
#define VIB_NODE    DT_ALIAS(vibration_motor)

struct gpio_output led = {
	.spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	.name = "LED",
};

struct gpio_output buzzer = {
	.spec = GPIO_DT_SPEC_GET(BUZZER_NODE, gpios),
	.name = "Buzzer",
};

struct gpio_output vibration_motor = {
	.spec = GPIO_DT_SPEC_GET(VIB_NODE, gpios),
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
