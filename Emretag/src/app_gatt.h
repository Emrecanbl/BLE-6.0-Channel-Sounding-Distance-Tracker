/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Application GATT service - button state in, output control out.
 *
 *  Distance values are not carried here; those go over the Ranging Service
 *  (RAS) provided by the nRF Connect SDK.
 */

#ifndef APP_GATT_H_
#define APP_GATT_H_

#include <stdint.h>

/** Notify the subscribed peer of a new button state.
 *
 *  Sends an HCI command and can block, so do not call from interrupt context.
 *  A no-op when no client has enabled notifications.
 */
int app_gatt_notify_button(uint8_t state);

#endif /* APP_GATT_H_ */
