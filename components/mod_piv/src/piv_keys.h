/*
 * PIV hardware backend: wires piv_backend_t to the secure element, mbedtls
 * (software 9D key + AES management key), NVS (certificates) and PinManager.
 */

#pragma once
#include "cdc_core/IModule.h"

namespace cdc::mod_piv {

/**
 * \brief Initializes the PIV backend for the assigned slot range.
 *
 * Cleans up FIDO2 leftovers orphaned by the slot-map shrink, loads or
 * first-boot-initializes the on-card state (GUID, CCC id, default AES-192
 * management key), and installs the backend so piv_applet() can serve APDUs.
 *
 * \param range Slot range assigned to mod_piv (ECC + RMEM 5..8).
 * \return true on success.
 */
bool piv_init(const core::IModule::SlotRange& range);

} // namespace cdc::mod_piv
