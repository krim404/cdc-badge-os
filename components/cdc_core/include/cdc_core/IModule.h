#pragma once

#include "cdc_core/IService.h"
#include <cstdint>

struct cJSON;

namespace cdc::ui {
    class IView;
}

namespace cdc::core {

/**
 * \brief Menu location for module registration.
 */
enum class MenuLocation : uint8_t {
    MAIN_MENU,       // Top-level main menu
    TOOLS_MENU,      // Under Tools submenu
    SETTINGS_MENU,   // Under Settings submenu
    BLUETOOTH_MENU,  // Under Bluetooth submenu (for BLE services)
    WIFI_MENU,       // Under WiFi submenu (for WiFi-related features)
    EXPERT_MENU      // Under Expert submenu (for advanced tools)
};

/**
 * \brief Menu item registered by a module.
 */
struct ModuleMenuItem {
    const char* label;              // Display label (use I18n for translation)
    uint8_t priority;               // Sort order (lower = higher in list)
    ui::IView* (*getView)();        // Factory function to get the view (push view on select)
    bool (*isVisible)();            // Optional visibility check (nullptr = always visible)
    const char* moduleName;         // Owner module name (set automatically)
    MenuLocation location;          // Where to show this item
    void (*onSelect)();             // Toggle/action callback (used when getView is nullptr)
};

/**
 * \brief Lock screen context menu item registered by a module.
 */
struct LockScreenContextItem {
    const char* (*getLabel)();      // Dynamic label getter (for state-dependent text)
    void (*callback)();             // Action when selected
    uint8_t priority;               // Sort order (lower = higher in list)
    const char* moduleName;         // Owner module name (set automatically)
};

/**
 * \brief Module interface that extends IService with module-specific features.
 *
 * Modules are self-contained features (TOTP, FIDO2, Password, etc.)
 * that can register menu items, serial commands, and views.
 */
class IModule : public IService {
public:
    struct SlotRequest {
        const char* mapName = nullptr;
        uint8_t minEccSlots = 0;
        uint16_t minRmemSlots = 0;
    };

    struct SlotRange {
        bool hasEcc = false;
        bool hasRmem = false;
        uint8_t eccStart = 0;
        uint8_t eccEnd = 0;
        uint16_t rmemStart = 0;
        uint16_t rmemEnd = 0;
        uint8_t moduleId = 0;
    };

    /**
     * \brief Returns the module version string.
     * \return Pointer to a null-terminated version string.
     */
    virtual const char* getVersion() const = 0;

    /**
     * \brief Per-module restore outcome reported by importBackup().
     *
     * Counts are best-effort tallies, not an all-or-nothing status: a module
     * imports each record independently and keeps going past failures.
     */
    struct BackupResult {
        uint16_t imported = 0;  ///< Records restored successfully.
        uint16_t failed = 0;    ///< Records skipped due to errors.
    };

    /**
     * \brief Exports this module's data as a JSON section for the backup file.
     *
     * Modules write semantic records (never raw slot/NVS blobs) into \p out so
     * the backup survives slot-layout and format changes. Default no-op: a
     * module that does not implement this is simply absent from the backup.
     *
     * \param out cJSON node (object or array) the module fills with its records.
     * \return true if the module produced exportable data, false to be skipped.
     */
    virtual bool exportBackup(cJSON* out) { (void)out; return false; }

    /**
     * \brief Restores this module's data from its JSON backup section.
     *
     * Best-effort: each record is created through the module's normal storage
     * path (fresh slot allocation); a record that cannot be restored is skipped
     * and counted, never aborting the whole restore. Default no-op.
     *
     * \param in cJSON node holding the module's previously exported section.
     * \return Tally of imported and failed records.
     */
    virtual BackupResult importBackup(const cJSON* in) { (void)in; return {}; }

    /**
     * \brief Returns module menu items.
     * \param items Output array to fill.
     * \param maxItems Maximum items to return.
     * \return Number of items written.
     */
    virtual uint8_t getMenuItems(ModuleMenuItem* items, uint8_t maxItems) {
        (void)items; (void)maxItems;
        return 0;
    }

    /**
     * \brief Returns the module's entry view (main view when selected from menu).
     * \return View instance or nullptr if no main view.
     */
    virtual ui::IView* getEntryView() { return nullptr; }

    /**
     * \brief Returns the module's lock screen context menu items.
     * \param items Output array to fill.
     * \param maxItems Maximum items to return.
     * \return Number of items written.
     */
    virtual uint8_t getLockScreenContextItems(LockScreenContextItem* items, uint8_t maxItems) {
        (void)items; (void)maxItems;
        return 0;
    }

    /**
     * \brief Called when device is unlocked.
     */
    virtual void onUnlock() {}

    /**
     * \brief Called when device is locked.
     */
    virtual void onLock() {}

    /**
     * \brief Called when USB is connected.
     */
    virtual void onUsbConnect() {}

    /**
     * \brief Called when USB is disconnected.
     */
    virtual void onUsbDisconnect() {}

    /**
     * \brief Called periodically (optional tick for background work).
     * \param nowMs Current timestamp in milliseconds.
     */
    virtual void onTick(uint32_t nowMs) { (void)nowMs; }

    /**
     * \brief Sets the slot range assigned by the module registry (from compile-time memory map).
     * \param range Assigned slot range.
     */
    virtual void setSlotRange(const SlotRange& range) { (void)range; }

    /**
     * \brief Returns slot requirements for this module (from compile-time memory map).
     * \return Slot request descriptor.
     */
    virtual SlotRequest getSlotRequest() const { return {}; }
};

} // namespace cdc::core
