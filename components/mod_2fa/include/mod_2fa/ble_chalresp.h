#pragma once

#include <cstdint>

namespace cdc::mod_2fa {

/**
 * \brief BLE GATT challenge-response transport.
 *
 * Exposes a small GATT service with a write characteristic (challenge) and a
 * notify characteristic (response). A connected host writes a challenge frame,
 * the badge optionally requires an on-device touch confirmation, computes the
 * raw HMAC via the registered `IChallengeResponder`, and notifies the result.
 *
 * Implemented purely through `IBluetoothController`; this file never touches
 * NimBLE directly. The write callback runs on the BLE host task and only
 * records the pending request; `ble_chalresp_tick()` (main task) performs the
 * confirmation and notification so the E-Paper UI is touched only there.
 */

/**
 * \brief Initializes the BLE CR subsystem and registers the GATT service.
 * \return `true` on success.
 */
bool ble_chalresp_init();

/**
 * \brief Tears down the BLE CR subsystem and removes GATT callbacks.
 */
void ble_chalresp_deinit();

/**
 * \brief Main-task tick: processes a pending challenge (confirm + notify).
 * \param nowMs Current uptime in milliseconds.
 */
void ble_chalresp_tick(uint32_t nowMs);

} // namespace cdc::mod_2fa
