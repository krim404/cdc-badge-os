/**
 * \file host_api_sysinfo.cpp
 * \brief Firmware identity / feature flags. Plugins use these to gate on
 *        the runtime they are loaded into - e.g. show a different UI for
 *        debug builds, or refuse to run on incompatible firmware revisions.
 */

#include "plugin_manager/host_api.h"
#include "cdc_core/feature_flags.h"
#include "cdc_core/CpuStats.h"

#include "esp_timer.h"

#include <cstdio>
#include <cstring>

#ifndef APP_NAME
#define APP_NAME "CDCBos"
#endif
#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

extern "C" {

int host_get_firmware_version(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    std::snprintf(out, out_size, "%s %s", APP_NAME, APP_VERSION);
    return HOST_OK;
}

int host_get_build_profile(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    // BUILD_PROFILE_BYTE is a compile-time constant in feature_flags.h.
    std::snprintf(out, out_size, "0x%02X", BUILD_PROFILE_BYTE);
    return HOST_OK;
}

bool host_feature_enabled(uint16_t feature_id)
{
    // Numeric IDs are loosely allocated:
    //  1 = FEATURE_USB, 2 = FEATURE_WIFI, 3 = FEATURE_BLE,
    //  4 = FEATURE_FIDO2, 5 = FEATURE_TOTP, 6 = FEATURE_GPG, 7 = DEBUG_MODE
    switch (feature_id) {
#ifdef FEATURE_USB
        case 1: return FEATURE_USB;
#endif
#ifdef FEATURE_WIFI
        case 2: return FEATURE_WIFI;
#endif
#ifdef FEATURE_BLE
        case 3: return FEATURE_BLE;
#endif
#ifdef FEATURE_FIDO2
        case 4: return FEATURE_FIDO2;
#endif
#ifdef FEATURE_TOTP
        case 5: return FEATURE_TOTP;
#endif
#ifdef FEATURE_GPG
        case 6: return FEATURE_GPG;
#endif
#ifdef DEBUG_MODE
        case 7: return DEBUG_MODE;
#endif
        default: return false;
    }
}

uint8_t host_cpu_load(void)
{
    // Recompute at most ~2x/s; cheap esp_timer check gates the task-state dump
    // so a per-frame caller (e.g. an LED effect) gets a stable cached value.
    static uint64_t lastIdle = 0;
    static uint64_t lastWall = 0;
    static bool primed = false;
    static uint8_t cached = 0;
    static constexpr uint64_t kRefreshUs = 500000ULL;

    uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (primed && (now - lastWall) < kRefreshUs) return cached;

    uint64_t idle, wall;
    if (!cdc::core::CpuStats::sample(idle, wall)) return cached;

    if (!primed) {
        lastIdle = idle;
        lastWall = wall;
        primed = true;
        return 0;
    }

    uint64_t denom = (wall - lastWall) * 2;  // two cores
    uint64_t idleDelta = idle - lastIdle;
    lastIdle = idle;
    lastWall = wall;
    if (denom == 0) return cached;
    if (idleDelta > denom) idleDelta = denom;  // clamp counter-wrap glitch
    cached = static_cast<uint8_t>(100 - (idleDelta * 100) / denom);
    return cached;
}

}  // extern "C"
