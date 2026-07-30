/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief User button (sw0) input.
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>

/** Button change callback.
 *
 *  Invoked from the system work queue - never from interrupt context - so it
 *  may call blocking Bluetooth APIs.
 *
 *  @param state       1 while the button is held down, 0 once released.
 *  @param press_count Number of presses since boot.
 */
typedef void (*button_cb_t)(uint8_t state, uint16_t press_count);

/** Configure sw0 as an interrupt-driven input.
 *
 *  Call this only once the rest of the system can cope with @p cb firing.
 */
int button_init(button_cb_t cb);

/** 1 while the button is held down, 0 once released. */
uint8_t button_get_state(void);

/** Number of presses since boot. */
uint16_t button_get_press_count(void);

#endif /* BUTTON_H_ */
