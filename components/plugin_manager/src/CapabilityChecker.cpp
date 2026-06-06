#include "plugin_manager/CapabilityChecker.h"
#include "plugin_manager/PluginGpioPolicy.h"
#include "plugin_manager/host_api.h"

#include <cctype>

namespace cdc::plugin_manager {

namespace {

// ESP-IDF NVS namespace identifier is bounded to 15 chars + NUL.
constexpr size_t NVS_NAMESPACE_MAX_LEN = 15;

// rmem slot name fits the on-chip 16-byte name field minus trailing NUL.
constexpr size_t RMEM_NAME_MAX_LEN = HOST_RMEM_NAME_MAX;

// Plugin linear memory and runtime structures live in PSRAM (Fast Interpreter).
// This range-checks the manifest's linear_memory_kb; the operand+frame stack is
// a fixed 64 KB allocated in Plugin.cpp.
constexpr uint32_t LINEAR_MEMORY_MIN_KB = 16;
constexpr uint32_t LINEAR_MEMORY_MAX_KB = 4096;

// 8-4-4-4-12 lowercase hex with dashes, e.g. "0000180a-0000-1000-8000-00805f9b34fb".
bool isValidUuid128(const std::string& s)
{
    static constexpr size_t kLen = 36;
    static constexpr size_t kDashPos[] = {8, 13, 18, 23};
    if (s.size() != kLen) return false;
    for (size_t pos : kDashPos) if (s[pos] != '-') return false;
    for (size_t i = 0; i < kLen; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = s[i];
        bool digit = (c >= '0' && c <= '9');
        bool lower = (c >= 'a' && c <= 'f');
        if (!digit && !lower) return false;
    }
    return true;
}

}  // namespace

CapabilityCheckResult CapabilityChecker::validate(const PluginManifest& m)
{
    if (m.api_level_major != HOST_API_LEVEL_MAJOR ||
        m.api_level_minor >  HOST_API_LEVEL_MINOR) {
        return { CapabilityResult::ApiLevelMismatch,
                 "plugin needs API " + m.host_api_level_min +
                 ", firmware provides " + std::string(HOST_API_LEVEL_STR) };
    }

    if (m.linear_memory_kb < LINEAR_MEMORY_MIN_KB ||
        m.linear_memory_kb > LINEAR_MEMORY_MAX_KB) {
        return { CapabilityResult::LinearMemoryOutOfRange,
                 "linear_memory_kb out of [" +
                     std::to_string(LINEAR_MEMORY_MIN_KB) + ", " +
                     std::to_string(LINEAR_MEMORY_MAX_KB) + "]" };
    }

    for (const std::string& name : m.capabilities.rmem) {
        if (name.empty() || name.size() > RMEM_NAME_MAX_LEN) {
            return { CapabilityResult::RmemNameInvalid,
                     "rmem name '" + name + "' must be 1-" +
                         std::to_string(RMEM_NAME_MAX_LEN) + " chars" };
        }
    }

    for (const std::string& name : m.capabilities.ecc) {
        if (name.empty() || name.size() > HOST_ECC_NAME_MAX) {
            return { CapabilityResult::EccNameInvalid,
                     "ecc name '" + name + "' must be 1-" +
                         std::to_string(HOST_ECC_NAME_MAX) + " chars" };
        }
    }

    for (uint8_t pin : m.capabilities.gpio_pins) {
        if (gpio_policy::isBlocked(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "GPIO " + std::to_string(pin) + " is reserved by firmware hardware" };
        }
        if (!gpio_policy::isAllowed(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "GPIO " + std::to_string(pin) + " not on plugin whitelist" };
        }
    }

    for (uint8_t pin : m.capabilities.pwm_pins) {
        if (gpio_policy::isBlocked(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "PWM pin " + std::to_string(pin) + " is reserved by firmware hardware" };
        }
        if (!gpio_policy::isAllowed(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "PWM pin " + std::to_string(pin) + " not on plugin whitelist" };
        }
    }

    for (uint8_t pin : m.capabilities.adc_pins) {
        if (gpio_policy::isBlocked(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "ADC pin " + std::to_string(pin) + " is reserved by firmware hardware" };
        }
        if (!gpio_policy::isAllowed(pin)) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "ADC pin " + std::to_string(pin) + " not on plugin whitelist" };
        }
    }

    for (uint8_t bus : m.capabilities.i2c_bus) {
        // Bus 0 is the internal charger + IO expander bus. Plugins must never
        // touch it - that bus controls power and IO expansion.
        if (bus == 0) {
            return { CapabilityResult::GpioPinNotAllowed,
                     "I2C bus 0 is reserved for internal hardware" };
        }
    }

    if (!m.capabilities.nvs_namespace.empty()) {
        const auto& ns = m.capabilities.nvs_namespace;
        if (ns.size() > NVS_NAMESPACE_MAX_LEN) {
            return { CapabilityResult::NvsNamespaceInvalid,
                     "nvs_namespace exceeds " +
                         std::to_string(NVS_NAMESPACE_MAX_LEN) + " chars" };
        }
        const bool prefixOk = ns.rfind("plg_", 0) == 0 ||
                              ns.rfind("plugin_", 0) == 0;
        if (!prefixOk) {
            return { CapabilityResult::NvsNamespaceInvalid,
                     "nvs_namespace must start with 'plg_' or 'plugin_'" };
        }
        for (char c : ns) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return { CapabilityResult::NvsNamespaceInvalid,
                         "nvs_namespace must be [a-z0-9_]" };
            }
        }
    } else if (!m.capabilities.rmem.empty()) {
        return { CapabilityResult::MissingNvsNamespace,
                 "nvs_namespace required when persistent state used" };
    }

    for (const auto& uuid : m.capabilities.ble_service_uuids) {
        if (!isValidUuid128(uuid)) {
            return { CapabilityResult::BleServiceUuidInvalid,
                     "ble_service_uuid '" + uuid +
                         "' not a 128-bit lowercase UUID" };
        }
    }

    return { CapabilityResult::Ok, {} };
}

}  // namespace cdc::plugin_manager
