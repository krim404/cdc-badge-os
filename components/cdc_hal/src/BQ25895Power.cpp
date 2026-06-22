/**
 * BQ25895 Power Manager HAL Implementation
 * Based on cdc-badge-os-legacy/components/cdc_badge/power_management.cpp
 *
 * Applies safe defaults on every boot (charger has no persistent storage).
 * Supports fast charging up to 1000mA for the 1200mAh LiPo battery.
 */

#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/II2cBus.h"
#include "cdc_hal/hw_config.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "BQ25895";

namespace cdc::hal {

/**
 * \brief BQ25895 register map constants.
 */
static constexpr uint8_t BQ_REG_INPUT_CTRL   = 0x00;  // Input source control
static constexpr uint8_t BQ_REG_ADC_CTRL     = 0x02;  // ADC control
static constexpr uint8_t BQ_REG_CHG_CTRL     = 0x03;  // Charge control (SYS_MIN, OTG)
static constexpr uint8_t BQ_REG_FAST_CHG     = 0x04;  // Fast charge current
static constexpr uint8_t BQ_REG_TIMER        = 0x07;  // Charge timer
static constexpr uint8_t BQ_REG_MISC         = 0x09;  // Misc (BATFET_DIS)
static constexpr uint8_t BQ_REG_SYS_STATUS   = 0x0B;  // System status
static constexpr uint8_t BQ_REG_FAULT        = 0x0C;  // Fault status
static constexpr uint8_t BQ_REG_BATV         = 0x0E;  // Battery voltage ADC
static constexpr uint8_t BQ_REG_SYSV         = 0x0F;  // System voltage ADC
static constexpr uint8_t BQ_REG_TS           = 0x10;  // TS ADC
static constexpr uint8_t BQ_REG_VBUS         = 0x11;  // VBUS voltage ADC
static constexpr uint8_t BQ_REG_ICHG         = 0x12;  // Charge current ADC
static constexpr uint8_t BQ_REG_VINDPM       = 0x13;  // VINDPM threshold
static constexpr uint8_t BQ_REG_VENDOR       = 0x14;  // Vendor/Part info

/**
 * \brief Charge current and safety threshold constants.
 */
static constexpr uint8_t  BQ_ICHG_STEP_MA = 64;       // REG04[6:0] step size
static constexpr uint16_t BQ_SYS_MIN_MV   = 3300;     // Minimum system voltage
static constexpr uint16_t CHARGE_CURRENT_SLOW = 512;  // Slow charge: 512mA
static constexpr uint16_t CHARGE_CURRENT_FAST = 1000; // Fast charge: 1000mA
static constexpr uint16_t CHARGE_CURRENT_MIN  = 64;
static constexpr uint16_t CHARGE_CURRENT_MAX  = 1024;  // Critical: max for 1200mAh LiPo

/**
 * \brief Battery voltage thresholds for state estimation.
 * LiPo battery characteristics (3.7V nominal, 4.2V max).
 */
static constexpr uint16_t BATTERY_MIN_MV   = 2800;  // Minimum usable voltage
static constexpr uint16_t BATTERY_EMPTY_MV = 3200;  // 0% calculation point
static constexpr uint16_t BATTERY_FULL_MV  = 4200;  // 100% calculation point
static constexpr uint16_t BATTERY_MAX_MV   = 4250;  // Maximum safe voltage
static constexpr uint16_t BATTERY_USB_PASSTHRU_MIN_MV = 4000;  // No-battery USB-passthrough range
static constexpr uint16_t BATTERY_USB_PASSTHRU_MAX_MV = BATTERY_MAX_MV;

/**
 * \brief Hold duration on the power/flash button that triggers ship mode.
 */
static constexpr uint32_t kPowerButtonLongPressMs = 3000;

/**
 * \brief Charger IRQ flag set by ISR and consumed in `update()`.
 */
static volatile bool charger_irq_pending = false;

/**
 * \brief GPIO interrupt handler for the charger IRQ pin.
 * \param arg Unused ISR argument.
 * \return void
 */
static void IRAM_ATTR charger_isr(void* arg) {
    (void)arg;
    charger_irq_pending = true;
}

/**
 * BQ25895 Power Manager Implementation
 */
class BQ25895Power : public IPowerManager {
public:
    BQ25895Power() = default;

    /**
     * \name IService implementation
     * \{
     */
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "power"; }
    /** \} */

    /**
     * \name IPowerManager implementation
     * \{
     */
    uint16_t getBatteryVoltage() const override;
    uint8_t getBatteryPercent() const override;
    bool isUsbConnected() const override;
    PowerSource getPowerSource() const override;
    ChargeStatus getChargeStatus() const override;
    bool isBatteryLow() const override;
    bool isBatteryCritical() const override;
    bool isBatteryPresent() const override;
    void setChargingEnabled(bool enabled) override;
    void enterShipMode() override;
    void setPreShipModeCallback(PreShipModeCallback cb) override { preShipModeCb_ = cb; }
    void update() override;
    void refresh() override;
    void prepareForSleep() override;
    void recoverFromSleep() override;
    /** \} */

private:
    /** \brief I2C register access helpers. */
    bool readReg(uint8_t reg, uint8_t* value) const;
    bool writeReg(uint8_t reg, uint8_t value);
    bool updateRegBits(uint8_t reg, uint8_t mask, uint8_t value, const char* label);

    /** \brief Internal helper methods. */
    void readChargerStatus();
    bool setChargeCurrentMa(uint16_t currentMa);

    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;

    /** \brief I2C device handles. */
    II2cBus* bus_ = nullptr;
    I2cDeviceHandle device_ = nullptr;

    /** \brief Kicks the charger watchdog timer. */
    void kickWatchdog();

    /** \brief Runtime charging state. */
    uint16_t currentChargeMa_ = CHARGE_CURRENT_SLOW;
    bool fastChargeEnabled_ = false;
    uint32_t lastWdtKickMs_ = 0;

    /** \brief Power-button long-press tracking for ship-mode entry. */
    bool powerButtonHeld_ = false;
    uint32_t powerButtonHoldStartMs_ = 0;

    /** \brief Callback run before the battery is disconnected. */
    PreShipModeCallback preShipModeCb_ = nullptr;

    /** \brief Cached charger state updated in `update()`. */
    mutable uint16_t cachedBatteryMv_ = 0;
    mutable ChargeStatus cachedChargeStatus_ = ChargeStatus::NOT_CHARGING;
    mutable bool cachedUsbConnected_ = false;
    mutable bool cachedBatteryPresent_ = false;

    /** \brief Previous status values for change-detection logging. */
    ChargeStatus prevChargeStatus_ = ChargeStatus::NOT_CHARGING;
    bool prevUsbConnected_ = false;
    bool prevBatteryPresent_ = false;
};

/**
 * \brief Reads one BQ25895 register.
 * \param reg Register address.
 * \param value Output register byte.
 * \return `true` on successful I2C read.
 */
bool BQ25895Power::readReg(uint8_t reg, uint8_t* value) const {
    if (!device_ || !value) return false;
    return bus_->readReg(device_, reg, value, 1) == ESP_OK;
}

/**
 * \brief Writes one BQ25895 register.
 * \param reg Register address.
 * \param value Register value.
 * \return `true` on successful I2C write.
 */
bool BQ25895Power::writeReg(uint8_t reg, uint8_t value) {
    if (!device_) return false;
    return bus_->writeReg(device_, reg, &value, 1) == ESP_OK;
}

/**
 * \brief Updates masked register bits while preserving remaining bits.
 * \param reg Register address.
 * \param mask Bit mask to update.
 * \param value New masked value.
 * \param label Log label for diagnostics.
 * \return `true` if operation succeeded or no change was needed.
 */
bool BQ25895Power::updateRegBits(uint8_t reg, uint8_t mask, uint8_t value, const char* label) {
    uint8_t current = 0;
    if (!readReg(reg, &current)) {
        LOG_E(TAG, "Read failed: %s", label);
        return false;
    }

    uint8_t newVal = (current & ~mask) | (value & mask);
    if (newVal == current) {
        LOG_D(TAG, "%s already set (0x%02X)", label, current);
        return true;
    }

    if (!writeReg(reg, newVal)) {
        LOG_E(TAG, "Write failed: %s", label);
        return false;
    }

    LOG_D(TAG, "%s: 0x%02X -> 0x%02X", label, current, newVal);
    return true;
}

/**
 * \brief Initializes charger hardware and applies safe boot defaults.
 * \return `true` if initialization succeeded.
 */
bool BQ25895Power::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    LOG_I(TAG, "Initializing BQ25895 power management");

    // Get I2C bus
    bus_ = getI2cBus0();
    if (!bus_ || bus_->getState() != core::ServiceState::INITIALIZED) {
        LOG_E(TAG, "I2C bus not initialized");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Add BQ25895 to I2C bus
    if (bus_->addDevice(BQ25895_ADDR, &device_) != ESP_OK) {
        LOG_E(TAG, "Failed to add BQ25895 to I2C0");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Verify chip ID (REG14[5:3] should be 0b111 for BQ25895)
    uint8_t vendor = 0;
    if (!readReg(BQ_REG_VENDOR, &vendor)) {
        LOG_E(TAG, "Failed to read vendor register");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    uint8_t partNumber = (vendor >> 3) & 0x07;
    if (partNumber != 7) {
        LOG_E(TAG, "BQ25895 not detected (got part=%d)", partNumber);
        state_ = core::ServiceState::ERROR;
        return false;
    }
    LOG_I(TAG, "BQ25895 detected (REG14=0x%02X)", vendor);

    // Apply safe defaults (charger has NO persistent storage!)

    // 1) Disable ILIM pin (REG00[6]=0) - use internal limit
    if (!updateRegBits(BQ_REG_INPUT_CTRL, (1 << 6), 0x00, "ILIM disable")) {
        return false;
    }

    // 2) Set minimum system voltage to 3.3V (REG03[3:1])
    //    Formula: SYS_MIN = 3.0V + code*0.1V -> code = 3 for 3.3V
    uint8_t sysMinCode = (BQ_SYS_MIN_MV - 3000) / 100;
    if (!updateRegBits(BQ_REG_CHG_CTRL, 0x0E, (uint8_t)(sysMinCode << 1), "SYS_MIN=3.3V")) {
        return false;
    }

    // 3) Disable OTG boost (REG03[5]=0)
    if (!updateRegBits(BQ_REG_CHG_CTRL, (1 << 5), 0x00, "OTG disable")) {
        return false;
    }

    // 4) Set initial charge current (default: slow charge 512mA)
    if (!setChargeCurrentMa(CHARGE_CURRENT_SLOW)) {
        return false;
    }

    // 5) Disable USB D+/D- detection (REG02[0]=0)
    //    This prevents the charger from pulling D+/D- during detection.
    if (!updateRegBits(BQ_REG_ADC_CTRL, (1 << 0), 0x00, "DPDM disable")) {
        return false;
    }

    // Configure charger IRQ (active-low, open-drain)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << CHG_IRQ_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    // Install ISR service (may already be installed by another driver)
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_E(TAG, "Failed to install ISR service: %s", esp_err_to_name(err));
        state_ = core::ServiceState::ERROR;
        return false;
    }
    gpio_isr_handler_add(CHG_IRQ_PIN, charger_isr, nullptr);

    // Configure power/flash button as input (active-low), polled in update()
    // for the ship-mode long-press.
    gpio_config_t btn_conf = {};
    btn_conf.pin_bit_mask = (1ULL << FLASH_BTN_PIN);
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&btn_conf);

    // Read initial status (clears any stale IRQ flags)
    readChargerStatus();

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "BQ25895 initialized");
    return true;
}

/**
 * \brief Starts power manager service state.
 * \return `true` if service is started after the call.
 */
bool BQ25895Power::start() {
    if (state_ == core::ServiceState::INITIALIZED ||
        state_ == core::ServiceState::STOPPED) {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops power manager service state.
 */
void BQ25895Power::stop() {
    if (state_ == core::ServiceState::STARTED) {
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Reads charger status/fault registers and refreshes cached state.
 */
void BQ25895Power::readChargerStatus() {
    uint8_t reg0b = 0, reg0c = 0;
    uint8_t chrgStat = 0;

    if (readReg(BQ_REG_SYS_STATUS, &reg0b)) {
        chrgStat = (reg0b >> 3) & 0x03;
        bool pgStat = (reg0b >> 2) & 0x01;
        // REG0B[0] VSYS_STAT: 1 = chip in VSYSMIN regulation (battery missing or
        // below VSYSMIN), 0 = battery is supplying VBAT >= VSYSMIN.
        bool vsysMinRegulation = (reg0b & 0x01) != 0;

        // Update cached status
        cachedUsbConnected_ = pgStat;

        switch (chrgStat) {
            case 0: cachedChargeStatus_ = ChargeStatus::NOT_CHARGING; break;
            case 1: cachedChargeStatus_ = ChargeStatus::PRE_CHARGE; break;
            case 2: cachedChargeStatus_ = ChargeStatus::FAST_CHARGE; break;
            case 3: cachedChargeStatus_ = ChargeStatus::CHARGE_DONE; break;
        }

        uint16_t vbat = getBatteryVoltage();
        uint8_t ichgr = 0;
        readReg(BQ_REG_ICHG, &ichgr);
        // ICHGR = (REG12[6:0]) * 50mA

        // Primary battery presence signal: VSYS_STAT bit.
        // Active charging (pre/fast) also implies a battery (overrides VSYS_STAT
        // during a charge cycle).
        if (cachedChargeStatus_ == ChargeStatus::FAST_CHARGE ||
            cachedChargeStatus_ == ChargeStatus::PRE_CHARGE) {
            cachedBatteryPresent_ = true;
        } else if (cachedUsbConnected_) {
            // With USB powering the system the battery is present iff the chip
            // is NOT in VSYSMIN regulation. CHARGE_DONE without battery keeps
            // VSYS_STAT=1; CHARGE_DONE with full battery keeps VSYS_STAT=0.
            cachedBatteryPresent_ = !vsysMinRegulation;
        } else {
            // On battery only (no USB): trust VBAT range and VSYS_STAT together.
            cachedBatteryPresent_ = !vsysMinRegulation &&
                                    (vbat >= BATTERY_MIN_MV && vbat <= BATTERY_MAX_MV);
        }

        // Correct "charge done" to "not charging" if no battery connected
        if (cachedChargeStatus_ == ChargeStatus::CHARGE_DONE && !cachedBatteryPresent_) {
            cachedChargeStatus_ = ChargeStatus::NOT_CHARGING;
        }

        // Only log on status change to avoid spam
        bool statusChanged = (cachedChargeStatus_ != prevChargeStatus_) ||
                             (cachedUsbConnected_ != prevUsbConnected_) ||
                             (cachedBatteryPresent_ != prevBatteryPresent_);

        if (statusChanged) {
            const char* chrgText[] = {"Not charging", "Pre-charge", "Fast charging", "Charge done"};
            uint16_t chargeMa = (ichgr & 0x7F) * 50;
            if (!cachedBatteryPresent_ && cachedUsbConnected_) {
                LOG_I(TAG, "USB connected, no battery (VBAT=%dmV, ICHG=%dmA)", vbat, chargeMa);
            } else {
                LOG_I(TAG, "Status: %s, USB=%d, VBAT=%dmV, ICHG=%dmA",
                      chrgText[static_cast<int>(cachedChargeStatus_)],
                      cachedUsbConnected_, vbat, chargeMa);
            }

            prevChargeStatus_ = cachedChargeStatus_;
            prevUsbConnected_ = cachedUsbConnected_;
            prevBatteryPresent_ = cachedBatteryPresent_;
        }
    }

    if (readReg(BQ_REG_FAULT, &reg0c) && reg0c != 0x00) {
        // Watchdog fault (0x80) when battery is full is expected behavior
        if (reg0c == 0x80 && chrgStat == 3 && cachedBatteryPresent_) {
            LOG_I(TAG, "Battery full");
        } else if (reg0c == 0x80) {
            // Watchdog expired after sleep - kick to resume charging
            LOG_D(TAG, "WDT expired (post-sleep), kicking");
            kickWatchdog();
        } else {
            // Real fault
            LOG_W(TAG, "Fault: 0x%02X", reg0c);
            cachedChargeStatus_ = ChargeStatus::FAULT;
        }
    }
}

/**
 * \brief Programs charging current setpoint.
 * \param currentMa Desired charging current in milliamps.
 * \return `true` if register update succeeded.
 */
bool BQ25895Power::setChargeCurrentMa(uint16_t currentMa) {
    // Clamp to valid range
    if (currentMa < CHARGE_CURRENT_MIN) currentMa = CHARGE_CURRENT_MIN;
    if (currentMa > CHARGE_CURRENT_MAX) currentMa = CHARGE_CURRENT_MAX;

    // Calculate register code: ICHG = code * 64mA
    uint8_t code = (uint8_t)(currentMa / BQ_ICHG_STEP_MA);

    // REG04[6:0] = charge current code
    if (!updateRegBits(BQ_REG_FAST_CHG, 0x7F, code, "ICHG")) {
        return false;
    }

    currentChargeMa_ = code * BQ_ICHG_STEP_MA;
    LOG_I(TAG, "Charge current set to %dmA (code=%d)", currentChargeMa_, code);
    return true;
}

/**
 * \brief Returns measured battery voltage in millivolts.
 * \return Battery voltage in mV, or `0` on read failure.
 */
uint16_t BQ25895Power::getBatteryVoltage() const {
    // Start ADC conversion if not running (REG02[7]=CONV_START)
    uint8_t reg02 = 0;
    if (readReg(BQ_REG_ADC_CTRL, &reg02) && !(reg02 & 0x80)) {
        const_cast<BQ25895Power*>(this)->writeReg(BQ_REG_ADC_CTRL, reg02 | 0x80);
        vTaskDelay(pdMS_TO_TICKS(10));  // Wait for ADC conversion
    }

    uint8_t batv = 0;
    if (!readReg(BQ_REG_BATV, &batv)) {
        return 0;
    }

    // Formula: VBAT = 2304mV + (BATV[6:0] * 20mV)
    uint8_t code = batv & 0x7F;
    cachedBatteryMv_ = 2304 + (code * 20);
    return cachedBatteryMv_;
}

/**
 * \brief Estimates battery percentage from measured voltage.
 * \return Battery level in percent.
 */
uint8_t BQ25895Power::getBatteryPercent() const {
    uint16_t mv = getBatteryVoltage();
    if (mv == 0) return 0;

    // Linear approximation: BATTERY_EMPTY_MV=0%, BATTERY_FULL_MV=100%
    if (mv <= BATTERY_EMPTY_MV) return 0;
    if (mv >= BATTERY_FULL_MV) return 100;

    return (uint8_t)(((uint32_t)(mv - BATTERY_EMPTY_MV) * 100) /
                     (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

/**
 * \brief Returns cached USB power presence.
 * \return `true` when USB input is detected.
 */
bool BQ25895Power::isUsbConnected() const {
    // Use cached value (updated in update())
    return cachedUsbConnected_;
}

/**
 * \brief Returns current active power source.
 * \return `PowerSource::USB` or `PowerSource::BATTERY`.
 */
PowerSource BQ25895Power::getPowerSource() const {
    return cachedUsbConnected_ ? PowerSource::USB : PowerSource::BATTERY;
}

/**
 * \brief Returns cached charge-state machine value.
 * \return Current charge status enum.
 */
ChargeStatus BQ25895Power::getChargeStatus() const {
    return cachedChargeStatus_;
}

/**
 * \brief Indicates low-battery threshold state.
 * \return `true` if battery level is below 20%.
 */
bool BQ25895Power::isBatteryLow() const {
    return getBatteryPercent() < 20;
}

/**
 * \brief Indicates critical-battery threshold state.
 * \return `true` if battery level is below 5%.
 */
bool BQ25895Power::isBatteryCritical() const {
    return getBatteryPercent() < 5;
}

/**
 * \brief Returns whether a battery is considered present.
 * \return `true` if battery presence is detected.
 */
bool BQ25895Power::isBatteryPresent() const {
    return cachedBatteryPresent_;
}

/**
 * \brief Enables or effectively disables charging current.
 * \param enabled Desired charging enable state.
 */
void BQ25895Power::setChargingEnabled(bool enabled) {
    if (enabled) {
        setChargeCurrentMa(fastChargeEnabled_ ? CHARGE_CURRENT_FAST : CHARGE_CURRENT_SLOW);
    } else {
        // Set minimum current to effectively disable
        setChargeCurrentMa(CHARGE_CURRENT_MIN);
    }
    LOG_I(TAG, "Charging %s", enabled ? "enabled" : "disabled");
}

/**
 * \brief Requests battery ship mode via BATFET disconnect.
 */
void BQ25895Power::enterShipMode() {
    // Show the ship-mode screen and let it fully render before cutting power.
    if (preShipModeCb_) {
        preShipModeCb_();
    }

    // Set BATFET_DIS (REG09[5]=1) to disconnect battery
    // System will only run from USB after this.
    // User must press PW ON / RESET to wake.
    uint8_t current = 0;
    if (!readReg(BQ_REG_MISC, &current)) {
        LOG_E(TAG, "Failed to read REG09");
        return;
    }

    if (!writeReg(BQ_REG_MISC, current | (1 << 5))) {
        LOG_E(TAG, "Failed to set BATFET_DIS");
        return;
    }

    LOG_I(TAG, "Entered shipping mode");
}

/**
 * \brief Resets charger watchdog timer.
 */
void BQ25895Power::kickWatchdog() {
    // REG03[6] = 1 resets the I2C watchdog timer
    // Silently kick without debug log (runs every 30s)
    uint8_t current = 0;
    if (readReg(BQ_REG_CHG_CTRL, &current)) {
        writeReg(BQ_REG_CHG_CTRL, current | (1 << 6));
    }
}

/**
 * \brief Periodic power-manager update handling watchdog and IRQ-driven refresh.
 */
void BQ25895Power::update() {
    // Proactive watchdog kick every 30s (WDT timeout is 40s)
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    if ((nowMs - lastWdtKickMs_) >= 30000) {
        lastWdtKickMs_ = nowMs;
        kickWatchdog();
    }

    // Power-button long-press enters ship mode (FLASH_BTN is active-low)
    if (gpio_get_level(FLASH_BTN_PIN) == 0) {
        if (!powerButtonHeld_) {
            powerButtonHeld_ = true;
            powerButtonHoldStartMs_ = nowMs;
        } else if ((nowMs - powerButtonHoldStartMs_) >= kPowerButtonLongPressMs) {
            powerButtonHeld_ = false;
            LOG_W(TAG, "Power button long-press, entering ship mode");
            enterShipMode();
        }
    } else {
        powerButtonHeld_ = false;
    }

    // Handle charger IRQ
    if (charger_irq_pending) {
        charger_irq_pending = false;
        readChargerStatus();
    }
}

/**
 * \brief Forces a synchronous re-read of charger status registers.
 */
void BQ25895Power::refresh() {
    readChargerStatus();
}

/**
 * \brief Disables the charger IRQ before light sleep.
 *
 * The sleep controller arms a level-triggered wakeup on the same pin; the
 * edge-triggered active-mode interrupt is disabled here to mirror the keypad.
 */
void BQ25895Power::prepareForSleep() {
    gpio_intr_disable(CHG_IRQ_PIN);
}

/**
 * \brief Restores charger IRQ handling and refreshes status after wakeup.
 *
 * Drops any stale IRQ flag, re-reads charger status (so cached USB presence is
 * current and an expired watchdog gets kicked), and re-arms the edge trigger.
 */
void BQ25895Power::recoverFromSleep() {
    gpio_intr_disable(CHG_IRQ_PIN);
    charger_irq_pending = false;
    readChargerStatus();
    gpio_set_intr_type(CHG_IRQ_PIN, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(CHG_IRQ_PIN);
}

/**
 * \brief Singleton power manager instance.
 */
static BQ25895Power g_powerManager;

/**
 * \brief Returns the singleton power manager instance.
 * \return Pointer to the global `IPowerManager` implementation.
 */
IPowerManager* getPowerManagerInstance() {
    return &g_powerManager;
}

} // namespace cdc::hal
