/**
 * \file ExtFeatureName.h
 * \brief Validation for external-feature names declared under the manifest
 *        `provides` capability and passed to the host_ext_feature_* API.
 *
 * Pure header-only logic (no ESP-IDF dependencies) so the native host test
 * suite can exercise it directly.
 */

#pragma once

#include <cstddef>

namespace cdc::plugin_manager {

/// Maximum external-feature name length including the trailing NUL.
inline constexpr size_t EXT_FEATURE_NAME_MAX = 33;

/// Maximum number of `provides` entries a single plugin manifest may declare.
inline constexpr size_t EXT_FEATURE_MAX_PER_PLUGIN = 4;

/**
 * \brief True if `name` is a well-formed external-feature name.
 *
 * Rules: 1..EXT_FEATURE_NAME_MAX-1 chars, first char [a-z], rest [a-z0-9_].
 */
inline bool isValidExtFeatureName(const char* name)
{
    if (!name || !name[0]) return false;
    if (name[0] < 'a' || name[0] > 'z') return false;
    size_t len = 0;
    for (const char* p = name; *p; ++p) {
        if (++len >= EXT_FEATURE_NAME_MAX) return false;
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace cdc::plugin_manager
