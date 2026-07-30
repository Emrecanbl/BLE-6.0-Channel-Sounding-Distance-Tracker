/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "button.h"

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

#define SW0_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

static button_cb_t user_cb;

/* 1 while the button is held down, 0 once it is released. */
static uint8_t button_state;
static uint16_t press_count;

/* The GATT and advertising calls the application makes from the callback can
 * block, so they must not run in the GPIO ISR. The ISR only samples the pin and
 * hands off to the work queue; the press count is incremented there too, so no
 * press is lost when several arrive before the work item gets to run.
 */
static void button_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (user_cb) {
		user_cb(button_state, press_count);
	}
}

static K_WORK_DEFINE(button_work, button_work_handler);

static void button_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Logical level: 1 when pressed, regardless of the active-low wiring. */
	button_state = (gpio_pin_get_dt(&button) > 0) ? 1 : 0;

	if (button_state) {
		press_count++;
	}

	k_work_submit(&button_work);
}

int button_init(button_cb_t cb)
{
	int err;

	user_cb = cb;

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button GPIO port is not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure button pin (err %d)", err);
		return err;
	}

	/* Both edges, otherwise the release is never seen and the reported state
	 * would stay stuck at 1.
	 */
	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (err) {
		LOG_ERR("Failed to configure button interrupt (err %d)", err);
		return err;
	}

	gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));

	err = gpio_add_callback_dt(&button, &button_cb_data);
	if (err) {
		LOG_ERR("Failed to add button callback (err %d)", err);
		return err;
	}

	return 0;
}

uint8_t button_get_state(void)
{
	return button_state;
}

uint16_t button_get_press_count(void)
{
	return press_count;
}
