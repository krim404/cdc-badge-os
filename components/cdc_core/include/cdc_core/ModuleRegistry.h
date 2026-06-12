#pragma once

#include "cdc_core/IModule.h"
#include <cstdint>
#include <cstddef>

namespace cdc::core {

/**
 * Module Registry - manages all registered modules
 *
 * Provides:
 * - Module registration and lifecycle management
 * - Menu item collection from all modules
 * - Event dispatch to modules (unlock, lock, USB, tick)
 */
// Module initializer function type
using ModuleInitFunc = void(*)();

/**
 * \brief Classified cause of a failed startModule() call.
 */
enum class ModuleStartFailure {
    SlotError,      ///< Module reported a slot-map error.
    UsbBudgetFull,  ///< HID interface budget is exhausted.
    Generic,        ///< Start failed for an unspecified reason.
};

class ModuleRegistry {
public:
    static constexpr uint8_t MAX_MODULES = 16;
    static constexpr uint8_t MAX_MENU_ITEMS = 32;
    static constexpr uint8_t MAX_INITIALIZERS = 16;

    static ModuleRegistry& instance();

    /**
     * Register a module initializer (called before system is fully ready)
     * The initializer will be called later by runAllInitializers()
     * \param initFunc Function to call for module init
     */
    void registerInitializer(ModuleInitFunc initFunc);

    /**
     * Run all registered module initializers
     * Called by main after system is ready (NVS, I18n, etc.)
     * Also performs NVS cleanup for removed modules.
     */
    void runAllInitializers();

    /**
     * Get the NVS namespace prefix for modules
     * All modules should use "mod_<name>" as their NVS namespace
     */
    static constexpr const char* NVS_PREFIX = "mod_";

    /**
     * Register a module
     * \param module Module instance (must remain valid)
     * \return true on success
     */
    bool registerModule(IModule* module);

    /**
     * Unregister a module by name
     * \param name Module name
     */
    void unregisterModule(const char* name);

    /**
     * Get a module by name
     * \param name Module name
     * \return Module pointer or nullptr
     */
    IModule* getModule(const char* name);

    /**
     * Initialize all registered modules
     * \return true if all succeeded
     */
    bool initAll();

    /**
     * Start all registered modules
     * \return true if all succeeded
     */
    bool startAll();

    /**
     * Start a single module by index (slot map validation enforced)
     * \return true on success
     */
    bool startModule(uint8_t index);

    /**
     * \brief Classifies why a preceding startModule() call failed.
     *
     * Applies the shared failure ladder: a module slot error takes precedence,
     * otherwise an exhausted USB HID budget, otherwise a generic failure.
     * \param index Module index that failed to start.
     * \return Classified failure cause.
     */
    ModuleStartFailure classifyStartFailure(uint8_t index) const;

    /**
     * Stop all registered modules
     */
    void stopAll();

    /**
     * Get all menu items for a specific location
     * \param location Menu location to filter
     * \param items Output array
     * \param maxItems Maximum items to return
     * \return Number of items written (sorted by priority)
     */
    uint8_t getMenuItems(MenuLocation location, ModuleMenuItem* items, uint8_t maxItems);

    /**
     * Get all lock screen context menu items from modules
     * \param items Output array
     * \param maxItems Maximum items to return
     * \return Number of items written (sorted by priority)
     */
    uint8_t getLockScreenContextItems(LockScreenContextItem* items, uint8_t maxItems);

    /**
     * Get total number of registered modules
     */
    uint8_t getModuleCount() const { return count_; }

    /**
     * Get module by index
     * \param index Module index (0 to count-1)
     * \return Module pointer or nullptr
     */
    IModule* getModuleAt(uint8_t index);

    // Event dispatch to all modules
    void dispatchUnlock();
    void dispatchLock();
    void dispatchUsbConnect();
    void dispatchUsbDisconnect();
    void dispatchTick(uint32_t nowMs);

    /**
     * \brief Returns a short status marker combining enabled flag and run state.
     * \param index Module index.
     * \return "[FAIL]" on slot error, "[ON]" enabled and started, "[--]"
     *         enabled but not started, "[OFF]" disabled.
     */
    const char* getModuleStatusLabel(uint8_t index) const;

    /**
     * Check if a module is enabled (will start on boot)
     * Uses module name for lookup - robust against index changes
     * \param index Module index
     * \return true if enabled
     */
    bool isModuleEnabled(uint8_t index) const;

    /**
     * Check if a module is enabled by name
     * \param name Module name
     * \return true if enabled
     */
    bool isModuleEnabledByName(const char* name) const;

    /**
     * Enable or disable a module (persistent across reboot)
     * Uses module name for storage - robust against index changes
     * \param index Module index
     * \param enabled true to enable, false to disable
     */
    void setModuleEnabled(uint8_t index, bool enabled);

    /**
     * Toggle module enabled state and save to NVS
     * \param index Module index
     * \return New enabled state
     */
    bool toggleModuleEnabled(uint8_t index);

    /**
     * Check if a module has a slot map error
     */
    bool hasModuleSlotError(uint8_t index) const;

    /**
     * Get slot map error message (if any)
     */
    const char* getModuleSlotError(uint8_t index) const;

    /**
     * Report a module error (called by modules at any time)
     * This marks the module as failed, stops it if running, and stores the error message.
     * The module will be treated as disabled until the error is cleared.
     * \param name Module name
     * \param message Error message (will be copied)
     */
    void reportModuleError(const char* name, const char* message);

    /**
     * Clear a module error (e.g., after successful retry)
     * \param name Module name
     */
    void clearModuleErrorByName(const char* name);

    /**
     * Retry a module that is in error state
     * Clears the error, re-initializes and starts the module.
     * \param index Module index
     * \return true if module started successfully, false if still in error
     */
    bool retryModule(uint8_t index);

private:
    ModuleRegistry() = default;

    /**
     * Clean up NVS data for modules that no longer exist
     * Compares saved module list with currently registered modules
     */
    void cleanupOrphanedModuleData();

    /**
     * Save current module list to NVS for future cleanup
     */
    void saveModuleList();

    IModule* modules_[MAX_MODULES] = {};
    uint8_t count_ = 0;

    ModuleInitFunc initializers_[MAX_INITIALIZERS] = {};
    uint8_t initCount_ = 0;

    struct ModuleError {
        bool hasError = false;
        char message[96] = {0};
    };

    ModuleError moduleErrors_[MAX_MODULES] = {};

    // Comma-separated list of disabled module names
    // Stored in NVS as string - robust against module order changes
    static constexpr size_t MAX_DISABLED_LIST_SIZE = 128;
    char disabledModules_[MAX_DISABLED_LIST_SIZE] = {0};

    /**
     * Load disabled modules list from NVS
     */
    void loadDisabledList();

    /**
     * Save disabled modules list to NVS
     */
    void saveDisabledList();

    void setModuleError(uint8_t index, const char* message);
    void clearModuleError(uint8_t index);
    bool applySlotRequest(IModule* module, uint8_t index);

    // Slot validation helpers for applySlotRequest
    bool validateSlotMap(const char* moduleName);
    bool validateEccRange(const char* mapName, const char* moduleName,
                          uint16_t minSlots, IModule::SlotRange& range,
                          uint8_t& moduleId);
    bool validateRmemRange(const char* mapName, const char* moduleName,
                           uint16_t minSlots, IModule::SlotRange& range,
                           uint8_t& moduleId);
    static void buildSlotErrorMessage(char* buffer, size_t bufSize,
                                      const char* errorType, const char* mapName);
};

// Convenience macro
#define CDC_MODULE(name) cdc::core::ModuleRegistry::instance().getModule(name)

} // namespace cdc::core
