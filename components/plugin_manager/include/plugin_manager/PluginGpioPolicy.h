/**
 * \file PluginGpioPolicy.h
 * \brief Single source of truth for plugin-accessible GPIO pins.
 *
 * Shared by CapabilityChecker (manifest validation) and GpioSerialCommands
 * (raw user shell). Both must use the same allow/block lists so the device
 * cannot be poked into an unsafe pin via either route.
 *
 * Mirrors hw_config.h - update both files together when board wiring changes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::plugin_manager::gpio_policy {

inline constexpr uint8_t BLOCKED[] = {
    0,    // FLASH_BTN (boot strapping + power button)
    1,    // EXP_IRQ (IO expander interrupt)
    8,    // EPD_LED (e-paper backlight)
    10,   // TR01_CS (TROPIC01 chip select)
    11,   // SPI_MISO (shared SPI bus)
    12,   // SPI_SCLK
    13,   // SPI_MOSI
    17,   // I2C0_SDA (charger + IO expander bus)
    18,   // I2C0_SCL
    19,   // USB D-
    20,   // USB D+
    21,   // CHG_DSEL (charger detection select)
    26, 27, 28, 29, 30, 31, 32,  // PSRAM/flash (not exposed via GPIO peripheral)
    33, 34, 35, 36, 37,  // Octal PSRAM data lines SPIIO4-7 + DQS (CONFIG_SPIRAM_MODE_OCT)
    39,   // CHG_IRQ
    41,   // EPD_CS
    42,   // EPD_BUSY
    45,   // EPD_DC
    46,   // EPD_RST
    47,   // I2C1_SDA (expansion bus - reachable via host_i2c_*, not as raw GPIO)
    48,   // I2C1_SCL
};

inline constexpr uint8_t ALLOWED[] = {
    2,    // Grove SIG0 (also RPi header)
    3,    // Grove SIG1 (also RPi header; ESP32-S3 strapping pin - boot mode)
    4,    // Header (ADC1_CH3)
    5,    // Header (ADC1_CH4)
    6,    // Header (ADC1_CH5)
    7,    // Header (ADC1_CH6)
    9,    // Header
    14,   // Header (ADC2)
    15,   // SAO GPIO1
    16,   // SAO GPIO2
    38,   // Header
    40,   // Header (JTAG TDO - safe unless USB-JTAG-Serial debugger is active)
    43,   // Header (UART0 TX - safe once serial console moves to CDC-only)
    44,   // Header (UART0 RX)
};

inline constexpr size_t BLOCKED_COUNT = sizeof(BLOCKED) / sizeof(BLOCKED[0]);
inline constexpr size_t ALLOWED_COUNT = sizeof(ALLOWED) / sizeof(ALLOWED[0]);

inline bool isBlocked(uint8_t pin)
{
    for (size_t i = 0; i < BLOCKED_COUNT; ++i) {
        if (BLOCKED[i] == pin) return true;
    }
    return false;
}

inline bool isAllowed(uint8_t pin)
{
    if (isBlocked(pin)) return false;
    for (size_t i = 0; i < ALLOWED_COUNT; ++i) {
        if (ALLOWED[i] == pin) return true;
    }
    return false;
}

}  // namespace cdc::plugin_manager::gpio_policy
