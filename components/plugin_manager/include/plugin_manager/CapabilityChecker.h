/**
 * \file CapabilityChecker.h
 * \brief Load-time validation of plugin capabilities + manifest sanity.
 *
 * Performs the static checks listed in docs/capabilities.md:
 *   - API-level compatibility
 *   - reserved R-Mem / ECC slots
 *   - GPIO whitelist
 *   - linear memory bounds
 *   - cross-plugin BLE / NVS / pin conflicts
 *
 * Runtime per-call capability checks live next to the actual host_api_*
 * implementations and use the manifest stored on the active Plugin instance.
 */

#pragma once

#include "plugin_manager/PluginManifest.h"

#include <string>

namespace cdc::plugin_manager {

enum class CapabilityResult {
    Ok,
    ApiLevelMismatch,
    LinearMemoryOutOfRange,
    RmemNameInvalid,
    EccNameInvalid,
    GpioPinNotAllowed,
    GpioPinAlreadyHeld,
    BleServiceUuidConflict,
    BleServiceUuidInvalid,
    NvsNamespaceConflict,
    NvsNamespaceInvalid,
    MissingNvsNamespace,
};

struct CapabilityCheckResult {
    CapabilityResult result;
    std::string detail;
    bool ok() const { return result == CapabilityResult::Ok; }
};

class CapabilityChecker {
public:
    [[nodiscard]] static CapabilityCheckResult validate(const PluginManifest& manifest);
};

}  // namespace cdc::plugin_manager
