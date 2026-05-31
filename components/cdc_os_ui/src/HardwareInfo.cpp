#include "cdc_os_ui/HardwareInfo.h"

#include "cdc_views/InfoView.h"
#include "cdc_ui/I18n.h"
#include "cdc_core/CpuStats.h"
#include "cdc_hal/II2cBus.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_hal/IEspHardware.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_hal/IBluetoothController.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#include <cstdarg>
#include <cstdio>

namespace cdc::ui {

/**
 * \brief Builds localized hardware info text into a caller-provided buffer.
 * \param buf Destination character buffer.
 * \param bufSize Size of destination buffer.
 * \return void
 */
static void buildHardwareInfoText(char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) return;
    size_t pos = 0;

    auto append = [&](const char* fmt, ...) {
        if (pos >= bufSize) return;
        va_list args;
        va_start(args, fmt);
        int written = vsnprintf(buf + pos, bufSize - pos, fmt, args);
        va_end(args);
        if (written > 0) {
            size_t w = static_cast<size_t>(written);
            pos += (w < (bufSize - pos)) ? w : (bufSize - pos - 1);
        }
    };

    const char* okText = ui::tr("core.ok");
    const char* failText = ui::tr("core.failed");
    const char* naText = ui::tr("core.hw_not_available");

    append("=== %s ===\n", ui::tr("core.hardware_info"));

    // I2C Bus (use bus 0 as primary)
    auto* i2c = hal::getI2cBus0();
    bool i2cOk = i2c && (i2c->getState() == core::ServiceState::INITIALIZED ||
                         i2c->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_i2c_bus"), i2cOk ? okText : failText);

    // Power Management
    auto* power = hal::getPowerManagerInstance();
    bool powerOk = power && (power->getState() == core::ServiceState::INITIALIZED ||
                             power->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_bq25895"), powerOk ? okText : failText);

    // Keypad
    auto* keypad = hal::getKeypadInstance();
    bool keypadOk = keypad && (keypad->getState() == core::ServiceState::INITIALIZED ||
                               keypad->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_tca9535"), keypadOk ? okText : failText);

    // Display
    auto* display = hal::getDisplayInstance();
    bool displayOk = display && (display->getState() == core::ServiceState::INITIALIZED ||
                                  display->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_display"), displayOk ? okText : failText);

    // TROPIC01
    auto* se = hal::getSecureElementInstance();
    bool seOk = se && (se->getState() == core::ServiceState::INITIALIZED ||
                       se->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_tropic01"), seOk ? okText : failText);
    append("%s: %s\n", ui::tr("core.hw_tr01_session"),
           (se && se->isSessionActive()) ? okText : naText);

    if (seOk) {
        uint8_t riscvVer[4] = {0};
        uint8_t spectVer[4] = {0};
        if (se->getFwVersion(riscvVer, spectVer)) {
            append("%s: %u.%u.%u.%u\n", ui::tr("core.hw_tr01_riscv_fw"),
                   riscvVer[3], riscvVer[2], riscvVer[1], riscvVer[0]);
            append("%s: %u.%u.%u.%u\n", ui::tr("core.hw_tr01_spect_fw"),
                   spectVer[3], spectVer[2], spectVer[1], spectVer[0]);
        } else {
            append("%s: %s\n", ui::tr("core.hw_tr01_riscv_fw"), naText);
            append("%s: %s\n", ui::tr("core.hw_tr01_spect_fw"), naText);
        }
        append("%s: %u B\n", ui::tr("core.hw_tr01_rmem_slot"), se->getRmemSlotSize());
    } else {
        append("%s: %s\n", ui::tr("core.hw_tr01_riscv_fw"), naText);
        append("%s: %s\n", ui::tr("core.hw_tr01_spect_fw"), naText);
        append("%s: %s\n", ui::tr("core.hw_tr01_rmem_slot"), naText);
    }

    // WiFi
    auto* wifi = hal::getWifiControllerInstance();
    bool wifiOk = wifi && (wifi->getState() == core::ServiceState::INITIALIZED ||
                           wifi->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_wifi"), wifiOk ? okText : naText);

    // Bluetooth
    auto* ble = hal::getBluetoothControllerInstance();
    bool bleOk = ble && (ble->getState() == core::ServiceState::INITIALIZED ||
                         ble->getState() == core::ServiceState::STARTED);
    append("%s: %s\n", ui::tr("core.hw_ble"), bleOk ? okText : naText);

    append("\n--- %s ---\n", ui::tr("core.hw_section_memory"));

    size_t freeHeap = esp_get_free_heap_size();
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t usedHeap = (totalHeap > freeHeap) ? (totalHeap - freeHeap) : 0;
    append("%s: %lu/%lu KB\n",
           ui::tr("core.hw_heap"),
           (unsigned long)(usedHeap / 1024),
           (unsigned long)(totalHeap / 1024));

    size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t intLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    append("DRAM: %lu/%lu KB free, largest %lu B\n",
           (unsigned long)(intFree / 1024),
           (unsigned long)(intTotal / 1024),
           (unsigned long)intLargest);

    size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    append("DMA: %lu KB free, largest %lu B\n",
           (unsigned long)(dmaFree / 1024),
           (unsigned long)dmaLargest);

    size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psramUsed = (psramTotal > psramFree) ? (psramTotal - psramFree) : 0;
    append("%s: %lu/%lu KB\n",
           ui::tr("core.hw_psram"),
           (unsigned long)(psramUsed / 1024),
           (unsigned long)(psramTotal / 1024));

    nvs_stats_t nvsStats;
    if (nvs_get_stats(nullptr, &nvsStats) == ESP_OK) {
        append("%s: %lu/%lu %s\n",
               ui::tr("core.hw_nvs"),
               (unsigned long)nvsStats.used_entries,
               (unsigned long)nvsStats.total_entries,
               ui::tr("core.hw_entries"));
    }

    append("\n--- %s ---\n", ui::tr("core.hw_section_runtime"));

    if (power) {
        const char* chg = (power->getChargeStatus() == hal::ChargeStatus::FAST_CHARGE ||
                           power->getChargeStatus() == hal::ChargeStatus::PRE_CHARGE)
                              ? ui::tr("core.hw_charging_suffix")
                              : "";
        append("%s: %u%%%s\n", ui::tr("core.hw_battery"), power->getBatteryPercent(), chg);
    } else {
        append("%s: %s\n", ui::tr("core.hw_battery"), naText);
    }

    float tempC = 0.0f;
    auto* espHw = hal::getEspHardwareInstance();
    if (espHw && espHw->getTemperatureC(&tempC)) {
        append("%s: %.1f C\n", ui::tr("core.hw_temp"), tempC);
    } else {
        append("%s: %s\n", ui::tr("core.hw_temp"), naText);
    }

    uint64_t uptimeS = esp_timer_get_time() / 1000000ULL;
    append("%s: %llu s\n", ui::tr("core.hw_uptime"), (unsigned long long)uptimeS);

    append("%s: %u%%\n", ui::tr("core.hw_cpu_load"), cdc::core::CpuStats::loadOverWindow());
}

/**
 * \brief Opens hardware info screen using shared info view.
 * \return void
 */
void showHardwareInfo() {
    static char hwInfo[512];
    buildHardwareInfoText(hwInfo, sizeof(hwInfo));
    showInfo(ui::tr("core.hardware_info"), hwInfo);
}

} // namespace cdc::ui
