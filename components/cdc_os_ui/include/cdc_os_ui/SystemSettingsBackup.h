#pragma once

#include "cdc_core/IModule.h"

struct cJSON;

namespace cdc::os_ui {

/**
 * \brief Export/import for OS-level settings that live in NVS without an owning
 *        module.
 *
 * The badge has user-configurable settings (WiFi, backlight, language, sleep
 * interval, timezone, badge text, module enable state) that are stored in NVS
 * by their respective owners (WifiHandlers, IDisplay, I18n, ISleepController,
 * IRtc, SettingsHandlers, ModuleRegistry) rather than under any IModule. This
 * helper serializes those settings into the backup container's top-level
 * "system" section and restores them best-effort, always through the owners'
 * public APIs (never by re-encoding NVS namespaces/keys here).
 *
 * Security-sensitive, device-bound data (badge PIN, duress PIN, TROPIC01 keys,
 * attestation) is intentionally excluded - it is not a setting. WiFi
 * credentials are included because the backup container is passphrase-encrypted.
 */
class SystemSettingsBackup {
public:
    /**
     * \brief Writes the user-configurable NVS settings into \p out.
     *
     * Adds a `schema_ver` plus the individual settings as JSON fields. Reads
     * every value via its owner's public getter; absent/unavailable services
     * are simply omitted.
     *
     * \param out JSON object node for the "system" section (must be non-null).
     * \return true if at least the schema marker was written.
     */
    static bool exportSystemSettings(cJSON* out);

    /**
     * \brief Restores the system settings from \p in best-effort.
     *
     * Each field is applied independently via its owner's public setter; a
     * missing, malformed or rejected field is skipped and counted, never
     * aborting the whole section (no migration). Backlight and language are
     * applied live; the rest take effect via the owners' persisted state.
     *
     * \param in JSON object node for the "system" section (must be non-null).
     * \return Per-field tally of applied vs. skipped settings.
     */
    static cdc::core::IModule::BackupResult importSystemSettings(const cJSON* in);
};

} // namespace cdc::os_ui
