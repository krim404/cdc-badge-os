#pragma once

#include "cdc_core/IService.h"
#include <cstdint>

namespace cdc::hal {

/**
 * Charging status
 */
enum class ChargeStatus : uint8_t {
    NOT_CHARGING,
    PRE_CHARGE,
    FAST_CHARGE,
    CHARGE_DONE,
    FAULT
};

/**
 * Power source
 */
enum class PowerSource : uint8_t {
    BATTERY,
    USB,
    UNKNOWN
};

/**
 * Power Manager interface (BQ25895)
 */
class IPowerManager : public core::IService {
public:
    /**
     * Callback invoked just before ship mode disconnects the battery. Lets the
     * UI render a transition screen so the user sees how to re-enable the badge.
     */
    using PreShipModeCallback = void (*)();

    virtual ~IPowerManager() = default;

    /**
     * Get battery voltage in mV
     */
    virtual uint16_t getBatteryVoltage() const = 0;

    /**
     * Get battery percentage (0-100)
     */
    virtual uint8_t getBatteryPercent() const = 0;

    /**
     * Check if USB is connected
     */
    virtual bool isUsbConnected() const = 0;

    /**
     * Get current power source
     */
    virtual PowerSource getPowerSource() const = 0;

    /**
     * Get charging status
     */
    virtual ChargeStatus getChargeStatus() const = 0;

    /**
     * Check if battery is low (<20%)
     */
    virtual bool isBatteryLow() const = 0;

    /**
     * Check if battery is critical (<5%)
     */
    virtual bool isBatteryCritical() const = 0;

    /**
     * Check if battery is physically connected
     */
    virtual bool isBatteryPresent() const = 0;

    /**
     * Enable/disable charging
     */
    virtual void setChargingEnabled(bool enabled) = 0;

    /**
     * Enable ship mode (deep power off)
     */
    virtual void enterShipMode() = 0;

    /**
     * Register a callback run immediately before ship mode disconnects the
     * battery. Pass nullptr to clear.
     */
    virtual void setPreShipModeCallback(PreShipModeCallback cb) = 0;

    /**
     * Update power status (poll from main loop)
     */
    virtual void update() = 0;

    /**
     * Force a synchronous re-read of charger status registers. Bypasses the
     * IRQ-driven cache so the caller observes the current hardware state.
     * Intended for paths that need an up-to-date reading on demand, e.g. the
     * lock-screen refresh.
     */
    virtual void refresh() = 0;

    /**
     * Arm the charger IRQ line as a light-sleep wakeup source
     * Disables the active-mode interrupt; the sleep controller configures the
     * level-triggered wakeup on the same pin.
     */
    virtual void prepareForSleep() = 0;

    /**
     * Restore charger IRQ handling after sleep wakeup
     * Refreshes the cached charger status (so USB presence is up to date) and
     * re-arms the edge-triggered interrupt.
     */
    virtual void recoverFromSleep() = 0;
};

// Factory function to get power manager instance
IPowerManager* getPowerManagerInstance();

} // namespace cdc::hal
