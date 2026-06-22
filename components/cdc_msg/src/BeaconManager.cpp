/**
 * \file
 * \brief Beacon advertising + persisted preference for message transfer.
 */

#include "cdc_msg/BeaconManager.h"
#include "cdc_msg/MessageProfile.h"

#include "cdc_core/Raii.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_log.h"
#include "nvs.h"

#include <cstring>

namespace cdc::msg {

namespace {
constexpr const char* TAG       = "MSG_BEACON";
constexpr const char* kNsMsg    = "msg";
constexpr const char* kKeyEna   = "ben";
constexpr const char* kKeyName  = "bname";
constexpr const char* kNsDisp   = "display";
constexpr const char* kKeyDisp  = "name";
constexpr const char* kFallback = "Badge";

/// Read an NVS string via a roomy temp buffer, then truncate into \p out.
bool readNvsStr(const char* ns, const char* key, char* out, size_t outSize)
{
    cdc::core::NvsScope nvs(ns, NVS_READONLY);
    if (!nvs) return false;
    char tmp[64] = {};
    size_t len = sizeof(tmp);
    if (nvs_get_str(nvs, key, tmp, &len) != ESP_OK || tmp[0] == '\0') return false;
    std::strncpy(out, tmp, outSize - 1);
    out[outSize - 1] = '\0';
    return true;
}
}  // namespace

void BeaconManager::load()
{
    cdc::core::NvsScope nvs(kNsMsg, NVS_READONLY);
    uint8_t ena = 1;  // default ON at initial rollout
    if (nvs) {
        nvs_get_u8(nvs, kKeyEna, &ena);
    }
    enabled_ = (ena != 0);
    resolveName();
}

void BeaconManager::resolveName()
{
    std::memset(name_, 0, sizeof(name_));
    if (readNvsStr(kNsMsg, kKeyName, name_, sizeof(name_))) return;   // custom name
    if (readNvsStr(kNsDisp, kKeyDisp, name_, sizeof(name_))) return;  // badge name
    std::strncpy(name_, kFallback, sizeof(name_) - 1);                // fallback
}

void BeaconManager::persistEnabled()
{
    cdc::core::NvsScope nvs(kNsMsg, NVS_READWRITE);
    if (!nvs) return;
    nvs_set_u8(nvs, kKeyEna, enabled_ ? 1 : 0);
    nvs.commit();
}

void BeaconManager::setEnabled(bool enabled)
{
    enabled_ = enabled;
    persistEnabled();

    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (enabled && ble && !ble->isEnabled()) {
        LOG_I(TAG, "Auto-enabling BLE for beacon");
        ble->enable();
    }
    reconcile();
}

void BeaconManager::setName(const char* name)
{
    cdc::core::NvsScope nvs(kNsMsg, NVS_READWRITE);
    if (nvs) {
        if (name && name[0] != '\0') {
            nvs_set_str(nvs, kKeyName, name);
        } else {
            nvs_erase_key(nvs, kKeyName);  // empty restores the default
        }
        nvs.commit();
    }
    resolveName();

    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (ble && ble->isEnabled() && applied_) {
        ble->setDeviceName(name_);
        ble->startAdvertising();  // re-advertise to pick up the new name
    }
}

bool BeaconManager::isActive() const
{
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    return enabled_ && applied_ && ble && ble->isEnabled();
}

void BeaconManager::apply()
{
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled()) return;
    ble->setDeviceName(name_);
    ble->addAdvertisingUuid(cdc::hal::BleUuid::from128(kMsgServiceUuid));
    ble->startAdvertising();
    applied_ = true;
    LOG_I(TAG, "Beacon active as '%s'", name_);
}

void BeaconManager::removeAdv()
{
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (ble) {
        ble->removeAdvertisingUuid(cdc::hal::BleUuid::from128(kMsgServiceUuid));
    }
    applied_ = false;
    LOG_I(TAG, "Beacon stopped");
}

void BeaconManager::bootApply()
{
    if (!enabled_) return;
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (!ble) return;
    // Populate the advertised name + UUID while BLE is still down, then request
    // enable. The controller defers the bring-up until the system is ready, so
    // the first advertising start already carries the service UUID.
    ble->setDeviceName(name_);
    ble->addAdvertisingUuid(cdc::hal::BleUuid::from128(kMsgServiceUuid));
    ble->enable();
    applied_ = true;
    LOG_I(TAG, "Beacon prepared as '%s'", name_);
}

void BeaconManager::reconcile()
{
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (!ble) return;

    if (!ble->isEnabled()) {
        applied_ = false;  // advertising not possible; re-apply when BLE returns
        return;
    }
    if (enabled_) {
        // A connection or a scan silently stops advertising. The peripheral side
        // re-advertises on disconnect, but a central that was beaconing does not,
        // so track the real radio state: re-apply whenever advertising is actually
        // down and no link is active (never mid-transfer).
        if (!ble->isAdvertising() && !ble->isConnected()) {
            apply();
        }
    } else if (applied_) {
        removeAdv();
    }
}

}  // namespace cdc::msg
