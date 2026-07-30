/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Bluetooth bring-up, advertising and Channel Sounding reflector role.
 */

#ifndef BLE_CS_H_
#define BLE_CS_H_

#include <stdint.h>

/** Enable the Bluetooth stack, load settings and start advertising. */
int ble_cs_start(void);

/** Publish a new button press count in the manufacturer-specific advertising
 *  data and refresh the advertising payload.
 *
 *  Sends an HCI command and can block, so do not call from interrupt context.
 */
int ble_cs_set_press_count(uint16_t count);

/** Configure the Channel Sounding reflector for each incoming connection.
 *
 *  Blocks forever; call it last from main().
 */
void ble_cs_run(void);

#endif /* BLE_CS_H_ */
