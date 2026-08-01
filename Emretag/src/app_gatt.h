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

/** Ask the connected phone to make itself findable.
 *
 *  Bumps a counter and notifies it, so a client can also see how many requests
 *  it missed. The phone clears the counter back to zero by writing 0 to the
 *  characteristic once it has handled or dismissed the request.
 *
 *  Sends an HCI command and can block; not for interrupt context.
 */
int app_gatt_notify_find_phone(void);

/** Called from the Bluetooth thread when the peer clears the fall alarm. */
typedef void (*app_gatt_fall_cleared_cb_t)(void);

/** Register who should be told that the peer cleared the fall alarm. */
void app_gatt_set_fall_cleared_cb(app_gatt_fall_cleared_cb_t cb);

/** Report that the tag has been dropped.
 *
 *  Same shape as the find-phone characteristic: a counter the peer clears by
 *  writing 0, which also stops the local alarm.
 */
int app_gatt_notify_fall(void);

#endif /* APP_GATT_H_ */
