#pragma once

#include "cdc_core/IModule.h"
#include "cJSON.h"

namespace cdc::ui {

/**
 * \brief Per-entry restore callback for importJsonArray().
 *
 * Receives one element of the backup array and performs the module-specific
 * field mapping plus upsert through the module's own storage path.
 *
 * \param entry One element of the JSON array (not guaranteed to be an object).
 * \param user Opaque pointer forwarded from importJsonArray().
 * \return true if the entry was stored, false if it was rejected/skipped.
 */
using BackupEntryHandler = bool (*)(const cJSON* entry, void* user);

/**
 * \brief Iterates a JSON backup array best-effort and tallies the outcome.
 *
 * Walks every element of \p array and invokes \p handler for each. A handler
 * returning false (or a non-object element) is counted as failed; the loop
 * never aborts early. The module-specific schema check and field mapping stay
 * in the caller/handler.
 *
 * \param array JSON array node (nullptr or non-array yields an empty tally).
 * \param handler Per-entry mapping/upsert callback.
 * \param user Opaque pointer forwarded to \p handler.
 * \return Tally of imported and failed records.
 */
inline cdc::core::IModule::BackupResult importJsonArray(const cJSON* array,
                                                         BackupEntryHandler handler,
                                                         void* user) {
    cdc::core::IModule::BackupResult result = {};
    if (!array || !handler || !cJSON_IsArray(array)) return result;

    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, array) {
        if (handler(entry, user)) {
            result.imported++;
        } else {
            result.failed++;
        }
    }
    return result;
}

} // namespace cdc::ui
