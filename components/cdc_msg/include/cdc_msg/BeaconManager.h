#pragma once

#include "cdc_msg/MessageTypes.h"

namespace cdc::msg {

/**
 * \brief Manages the message-transfer beacon.
 *
 * Owns a persisted on/off preference (NVS namespace "msg", default ON at
 * initial rollout), a configurable display name (default = the badge name from
 * NVS "display"/"name"), and the standard GAP advertising of the service UUID
 * plus the Complete Local Name. No proprietary manufacturer data.
 */
class BeaconManager {
public:
    /// \brief Load the persisted preference and resolve the name. Call once at init.
    void load();

    /**
     * \brief Enable or disable the beacon and persist it.
     *
     * Enabling while Bluetooth is off auto-enables Bluetooth first. Applies or
     * removes advertising of the service UUID immediately.
     */
    void setEnabled(bool enabled);

    /// \return the persisted on/off preference.
    bool isEnabled() const { return enabled_; }

    /// \return true if the beacon is actually advertising right now.
    bool isActive() const;

    /// \brief Set and persist the display name; an empty name restores the default.
    void setName(const char* name);

    /// \return the resolved display name (custom -> badge name -> fallback).
    const char* getName() const { return name_; }

    /// \brief Reconcile advertising with the preference and BLE power state. Call from tick().
    void reconcile();

private:
    void resolveName();
    void persistEnabled();
    void apply();
    void removeAdv();

    bool enabled_ = true;   ///< Default ON at initial rollout.
    bool applied_ = false;  ///< Our service UUID is currently advertised.
    char name_[kNameBufSize] = {};
};

}  // namespace cdc::msg
