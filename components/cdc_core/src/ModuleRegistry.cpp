#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/TropicSlotMap.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/EventBus.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <memory>

static const char* TAG = "ModuleReg";

namespace cdc::core {

/**
 * \brief Returns the singleton module registry instance.
 * \return Registry singleton reference.
 */
ModuleRegistry& ModuleRegistry::instance() {
    static ModuleRegistry* instance = new ModuleRegistry();
    return *instance;
}

/**
 * \brief Registers a deferred module initializer callback.
 * \param initFunc Initializer function to execute during startup.
 */
void ModuleRegistry::registerInitializer(ModuleInitFunc initFunc) {
    if (!initFunc) return;

    if (initCount_ >= MAX_INITIALIZERS) {
        LOG_E(TAG, "Module initializer registry full");
        return;
    }

    initializers_[initCount_++] = initFunc;
}

/**
 * \brief Executes all registered initializers and post-registration housekeeping.
 */
void ModuleRegistry::runAllInitializers() {
    // Modules access TropicStorage during their init() (slot-map lookups,
    // initial chunk reads). Hard-fail loudly if main() forgot to bring it
    // up first instead of letting modules run on undefined state.
    auto storageState = TropicStorage::instance().getState();
    if (storageState != ServiceState::STARTED) {
        LOG_E(TAG, "TropicStorage not STARTED (state=%d) before module init - aborting",
              static_cast<int>(storageState));
        return;
    }

    LOG_I(TAG, "Running %d module initializers", initCount_);

    for (uint8_t i = 0; i < initCount_; i++) {
        if (initializers_[i]) {
            initializers_[i]();
        }
    }

    // Load disabled modules list from NVS (name-based, robust against order changes)
    loadDisabledList();

    // Initializers only init() their modules; exactly the enabled set is
    // started here. The stop pass enforces that contract for any module
    // started out-of-band, so a disabled module can never hold resources
    // (e.g. USB interface slots) across boot.
    for (uint8_t i = 0; i < count_; i++) {
        if (!isModuleEnabled(i) && modules_[i]->getState() == ServiceState::STARTED) {
            LOG_W(TAG, "Stopping disabled module '%s' (started during init)", modules_[i]->getName());
            modules_[i]->stop();
        }
    }
    for (uint8_t i = 0; i < count_; i++) {
        if (isModuleEnabled(i) && !startModule(i)) {
            LOG_W(TAG, "Enabled module '%s' failed to start", modules_[i]->getName());
        }
    }

    // After all modules are registered, clean up orphaned NVS data
    cleanupOrphanedModuleData();

    // Save current module list for next boot
    saveModuleList();
}

/**
 * \brief Registers a module instance in the runtime registry.
 * \param module Module instance to register.
 * \return `true` if registration succeeded.
 */
bool ModuleRegistry::registerModule(IModule* module) {
    if (!module) {
        LOG_E(TAG, "Cannot register null module");
        return false;
    }

    if (count_ >= MAX_MODULES) {
        LOG_E(TAG, "Module registry full, cannot register '%s'", module->getName());
        return false;
    }

    // Check for duplicate
    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(modules_[i]->getName(), module->getName()) == 0) {
            LOG_W(TAG, "Module '%s' already registered", module->getName());
            return false;
        }
    }

    modules_[count_++] = module;
    clearModuleError(count_ - 1);
    (void)applySlotRequest(module, count_ - 1);
    LOG_I(TAG, "Registered module '%s' v%s", module->getName(), module->getVersion());
    return true;
}

/**
 * \brief Unregisters a module by name.
 * \param name Module name.
 */
void ModuleRegistry::unregisterModule(const char* name) {
    if (!name) return;

    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(modules_[i]->getName(), name) == 0) {
            // Shift remaining modules
            for (uint8_t j = i; j < count_ - 1; j++) {
                modules_[j] = modules_[j + 1];
            }
            modules_[--count_] = nullptr;
            LOG_I(TAG, "Unregistered module '%s'", name);
            return;
        }
    }
}

/**
 * \brief Looks up a module by name.
 * \param name Module name.
 * \return Pointer to module or `nullptr` if not found.
 */
IModule* ModuleRegistry::getModule(const char* name) {
    if (!name) return nullptr;

    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(modules_[i]->getName(), name) == 0) {
            return modules_[i];
        }
    }
    return nullptr;
}

/**
 * \brief Returns module pointer at registry index.
 * \param index Module index.
 * \return Pointer to module or `nullptr` if out of range.
 */
IModule* ModuleRegistry::getModuleAt(uint8_t index) {
    if (index >= count_) return nullptr;
    return modules_[index];
}

/**
 * \brief Calls `init()` on all registered modules.
 * \return `true` if all modules initialized successfully.
 */
bool ModuleRegistry::initAll() {
    bool allOk = true;
    for (uint8_t i = 0; i < count_; i++) {
        if (!modules_[i]->init()) {
            LOG_E(TAG, "Failed to init module '%s'", modules_[i]->getName());
            allOk = false;
        }
    }
    return allOk;
}

/**
 * \brief Starts all enabled modules.
 * \return `true` if all enabled modules started successfully.
 */
bool ModuleRegistry::startAll() {
    bool allOk = true;
    for (uint8_t i = 0; i < count_; i++) {
        // Skip disabled modules
        if (!isModuleEnabled(i)) {
            LOG_I(TAG, "Module '%s' is disabled, skipping start", modules_[i]->getName());
            continue;
        }

        if (!startModule(i)) {
            allOk = false;
        }
    }
    return allOk;
}

/**
 * \brief Starts a single module by index.
 * \param index Module index.
 * \return `true` if module is started after the call.
 */
bool ModuleRegistry::startModule(uint8_t index) {
    if (index >= count_) return false;
    IModule* module = modules_[index];
    if (!module) return false;

    if (hasModuleSlotError(index)) {
        LOG_E(TAG, "Module '%s' blocked: %s", module->getName(),
              getModuleSlotError(index) ? getModuleSlotError(index) : "slot map error");
        return false;
    }

    if (module->getState() == ServiceState::INITIALIZED ||
        module->getState() == ServiceState::STOPPED) {
        if (!module->start()) {
            LOG_E(TAG, "Failed to start module '%s'", module->getName());
            return false;
        }
    }

    return true;
}

/**
 * \brief Resolves the cause of a failed startModule() call.
 * \param index Module index that failed to start.
 * \return Classified failure cause.
 */
ModuleStartFailure ModuleRegistry::classifyStartFailure(uint8_t index) const {
    if (getModuleSlotError(index)) {
        return ModuleStartFailure::SlotError;
    }
    // Best-effort attribution: reflects current global HID occupancy, not a
    // precise per-call cause for this specific module start.
    if (UsbManager::instance().hidSlotsFull()) {
        return ModuleStartFailure::UsbBudgetFull;
    }
    return ModuleStartFailure::Generic;
}

/**
 * \brief Stops all currently started modules.
 */
void ModuleRegistry::stopAll() {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->stop();
        }
    }
}

/**
 * \brief Collects menu items from started modules for a given location.
 * \param location Target menu location.
 * \param items Output array for aggregated menu items.
 * \param maxItems Maximum writable entries in `items`.
 * \return Number of returned menu items.
 */
uint8_t ModuleRegistry::getMenuItems(MenuLocation location, ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    uint8_t totalCount = 0;

    // Collect items from all modules
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() != ServiceState::STARTED) continue;

        ModuleMenuItem moduleItems[8] = {};
        uint8_t count = modules_[i]->getMenuItems(moduleItems, 8);

        for (uint8_t j = 0; j < count && totalCount < maxItems; j++) {
            if (moduleItems[j].location == location) {
                // Check visibility
                if (moduleItems[j].isVisible && !moduleItems[j].isVisible()) {
                    continue;
                }
                // Set module name
                moduleItems[j].moduleName = modules_[i]->getName();
                items[totalCount++] = moduleItems[j];
            }
        }
    }

    // Sort by priority (bubble sort, small array)
    for (uint8_t i = 0; i < totalCount; i++) {
        for (uint8_t j = i + 1; j < totalCount; j++) {
            if (items[j].priority < items[i].priority) {
                ModuleMenuItem tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }
        }
    }

    return totalCount;
}

/**
 * \brief Dispatches unlock lifecycle event to started modules.
 */
void ModuleRegistry::dispatchUnlock() {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->onUnlock();
        }
    }
}

/**
 * \brief Dispatches lock lifecycle event to started modules.
 */
void ModuleRegistry::dispatchLock() {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->onLock();
        }
    }
}

/**
 * \brief Dispatches USB-connect lifecycle event to started modules.
 */
void ModuleRegistry::dispatchUsbConnect() {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->onUsbConnect();
        }
    }
}

/**
 * \brief Dispatches USB-disconnect lifecycle event to started modules.
 */
void ModuleRegistry::dispatchUsbDisconnect() {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->onUsbDisconnect();
        }
    }
}

/**
 * \brief Dispatches periodic tick callback to started modules.
 * \param nowMs Current system time in milliseconds.
 */
void ModuleRegistry::dispatchTick(uint32_t nowMs) {
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() == ServiceState::STARTED) {
            modules_[i]->onTick(nowMs);
        }
    }
}

/**
 * \brief Collects lock-screen context actions from started modules.
 * \param items Output array for context items.
 * \param maxItems Maximum writable entries in `items`.
 * \return Number of returned context items.
 */
uint8_t ModuleRegistry::getLockScreenContextItems(LockScreenContextItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    uint8_t totalCount = 0;

    // Collect items from all modules
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->getState() != ServiceState::STARTED) continue;

        LockScreenContextItem moduleItems[4];
        uint8_t count = modules_[i]->getLockScreenContextItems(moduleItems, 4);

        for (uint8_t j = 0; j < count && totalCount < maxItems; j++) {
            // Set module name
            moduleItems[j].moduleName = modules_[i]->getName();
            items[totalCount++] = moduleItems[j];
        }
    }

    // Sort by priority (bubble sort, small array)
    for (uint8_t i = 0; i < totalCount; i++) {
        for (uint8_t j = i + 1; j < totalCount; j++) {
            if (items[j].priority < items[i].priority) {
                LockScreenContextItem tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }
        }
    }

    return totalCount;
}

/**
 * \brief NVS garbage collection for removed modules.
 */

static constexpr const char* MODULES_NVS_NAMESPACE = "modules";
static constexpr const char* MODULES_NVS_KEY = "list";
static constexpr const char* MODULES_NVS_KEY_DISABLED = "disabled";
static constexpr size_t MAX_MODULE_LIST_SIZE = 256;

/**
 * \brief Removes persisted NVS data for modules no longer present in firmware.
 */
void ModuleRegistry::cleanupOrphanedModuleData() {
    NvsScope handle(MODULES_NVS_NAMESPACE, NVS_READONLY);
    if (!handle) {
        LOG_I(TAG, "No previous module list found (first boot)");
        return;
    }

    char savedList[MAX_MODULE_LIST_SIZE] = {0};
    size_t len = sizeof(savedList);
    esp_err_t err = nvs_get_str(handle, MODULES_NVS_KEY, savedList, &len);
    handle.close();

    if (err != ESP_OK || len == 0) {
        LOG_I(TAG, "No saved module list");
        return;
    }

    LOG_I(TAG, "Checking for orphaned module data...");

    // Parse comma-separated list and check each module
    char* saveptr = nullptr;
    char* token = strtok_r(savedList, ",", &saveptr);

    while (token) {
        // Skip empty tokens
        if (strlen(token) == 0) {
            token = strtok_r(nullptr, ",", &saveptr);
            continue;
        }

        // Check if this module is currently registered
        bool found = false;
        for (uint8_t i = 0; i < count_; i++) {
            if (strcmp(modules_[i]->getName(), token) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            // Module no longer exists - erase its NVS namespace
            char nsName[20];
            snprintf(nsName, sizeof(nsName), "%s%s", NVS_PREFIX, token);

            LOG_W(TAG, "Module '%s' removed - erasing NVS namespace '%s'", token, nsName);

            NvsScope modHandle(nsName, NVS_READWRITE);
            if (modHandle) {
                nvs_erase_all(modHandle);
                modHandle.commit();
                LOG_I(TAG, "Erased NVS data for removed module '%s'", token);
            }
        }

        token = strtok_r(nullptr, ",", &saveptr);
    }
}

/**
 * \brief Persists current registered module-name list to NVS.
 */
void ModuleRegistry::saveModuleList() {
    if (count_ == 0) {
        LOG_I(TAG, "No modules to save");
        return;
    }

    // Build comma-separated list of module names
    char moduleList[MAX_MODULE_LIST_SIZE] = {0};
    size_t offset = 0;

    for (uint8_t i = 0; i < count_; i++) {
        const char* name = modules_[i]->getName();
        size_t nameLen = strlen(name);

        // Check if it fits
        if (offset + nameLen + 2 > sizeof(moduleList)) {
            LOG_W(TAG, "Module list too long, truncating");
            break;
        }

        if (offset > 0) {
            moduleList[offset++] = ',';
        }
        memcpy(moduleList + offset, name, nameLen);
        offset += nameLen;
    }
    moduleList[offset] = '\0';

    NvsScope handle(MODULES_NVS_NAMESPACE, NVS_READWRITE);
    if (handle) {
        nvs_set_str(handle, MODULES_NVS_KEY, moduleList);
        handle.commit();
        LOG_I(TAG, "Saved module list: %s", moduleList);
    } else {
        LOG_E(TAG, "Failed to save module list");
    }
}

/**
 * \brief Name-based module enable/disable persistence helpers.
 */

/**
 * \brief Loads persisted disabled-module list from NVS.
 */
void ModuleRegistry::loadDisabledList() {
    bool loaded = false;
    {
        NvsScope handle(MODULES_NVS_NAMESPACE, NVS_READONLY);
        if (handle) {
            size_t len = sizeof(disabledModules_);
            if (nvs_get_str(handle, MODULES_NVS_KEY_DISABLED, disabledModules_, &len) == ESP_OK) {
                LOG_I(TAG, "Loaded disabled modules: %s", disabledModules_);
                loaded = true;
            }
        }
    }
    if (loaded) return;

    // No NVS entry: apply factory defaults from each module's isDefaultEnabled().
    disabledModules_[0] = '\0';
    size_t offset = 0;
    for (uint8_t i = 0; i < count_; i++) {
        if (modules_[i]->isDefaultEnabled()) continue;
        const char* name = modules_[i]->getName();
        size_t nameLen = strlen(name);
        size_t needed = (offset > 0 ? 1 : 0) + nameLen + 1;
        if (offset + needed > sizeof(disabledModules_)) break;
        if (offset > 0) disabledModules_[offset++] = ',';
        memcpy(disabledModules_ + offset, name, nameLen);
        offset += nameLen;
        disabledModules_[offset] = '\0';
    }
    if (disabledModules_[0] != '\0') {
        LOG_I(TAG, "Applied factory-default disabled list: %s", disabledModules_);
        saveDisabledList();
    }
}

/**
 * \brief Saves disabled-module list to NVS.
 */
void ModuleRegistry::saveDisabledList() {
    NvsScope handle(MODULES_NVS_NAMESPACE, NVS_READWRITE);
    if (handle) {
        nvs_set_str(handle, MODULES_NVS_KEY_DISABLED, disabledModules_);
        handle.commit();
        LOG_I(TAG, "Saved disabled modules: %s", disabledModules_);
    } else {
        LOG_E(TAG, "Failed to save disabled modules list");
    }
}

/**
 * \brief Checks whether a module name is currently enabled.
 * \param name Module name.
 * \return `true` if module is enabled.
 */
bool ModuleRegistry::isModuleEnabledByName(const char* name) const {
    if (!name || name[0] == '\0') return true;
    if (disabledModules_[0] == '\0') return true;  // No disabled modules

    // Search for name in comma-separated list
    size_t nameLen = strlen(name);
    const char* ptr = disabledModules_;

    while (*ptr) {
        // Skip leading commas
        while (*ptr == ',') ptr++;
        if (*ptr == '\0') break;

        // Find end of current token
        const char* end = ptr;
        while (*end && *end != ',') end++;
        size_t tokenLen = end - ptr;

        // Compare
        if (tokenLen == nameLen && strncmp(ptr, name, nameLen) == 0) {
            return false;  // Found in disabled list
        }

        ptr = end;
    }

    return true;  // Not in disabled list = enabled
}

/**
 * \brief Returns a short status marker combining enabled flag and run state.
 * \param index Module index.
 * \return Static marker string ([FAIL]/[ON]/[--]/[OFF]).
 */
const char* ModuleRegistry::getModuleStatusLabel(uint8_t index) const {
    if (index >= count_) return "[--]";
    if (hasModuleSlotError(index)) return "[FAIL]";
    if (!isModuleEnabled(index)) return "[OFF]";
    return modules_[index]->getState() == ServiceState::STARTED ? "[ON]" : "[--]";
}

/**
 * \brief Checks whether module at index is enabled.
 * \param index Module index.
 * \return `true` if enabled.
 */
bool ModuleRegistry::isModuleEnabled(uint8_t index) const {
    if (index >= count_) return false;
    return isModuleEnabledByName(modules_[index]->getName());
}

/**
 * \brief Removes a name from a comma-separated list, in place.
 *
 * Iterates through tokens of the input list and writes back the result with
 * any token equal to `name` filtered out. Other tokens keep their original
 * order. The destination must have capacity for at least `capacity` bytes
 * (including the terminating null) and may alias `list`.
 *
 * \param list Source comma-separated list (must be null-terminated).
 * \param name Token to remove (case-sensitive, exact match).
 * \param dest Destination buffer to receive the filtered list.
 * \param capacity Size of `dest` in bytes including the null terminator.
 */
static void removeNameFromList(const char* list, const char* name,
                               char* dest, size_t capacity) {
    if (capacity == 0) return;

    auto tmp = std::make_unique<char[]>(capacity);
    tmp[0] = '\0';
    size_t newOffset = 0;
    const size_t nameLen = strlen(name);
    const size_t maxOffset = capacity;

    const char* ptr = list;
    while (*ptr) {
        while (*ptr == ',') ptr++;
        if (*ptr == '\0') break;

        const char* end = ptr;
        while (*end && *end != ',') end++;
        size_t tokenLen = static_cast<size_t>(end - ptr);

        const bool matches = (tokenLen == nameLen &&
                              strncmp(ptr, name, nameLen) == 0);
        if (!matches) {
            if (newOffset > 0 && newOffset + 1 < maxOffset) {
                tmp[newOffset++] = ',';
            }
            if (newOffset + tokenLen < maxOffset) {
                memcpy(tmp.get() + newOffset, ptr, tokenLen);
                newOffset += tokenLen;
            }
        }

        ptr = end;
    }
    tmp[newOffset] = '\0';
    memcpy(dest, tmp.get(), newOffset + 1);
    dest[capacity - 1] = '\0';
}

/**
 * \brief Appends a name to a comma-separated list, in place.
 *
 * Adds `name` to `list`, inserting a leading comma when the list is non-empty.
 * No action is taken when the resulting string would exceed `capacity`.
 *
 * \param list In/out buffer holding the comma-separated list.
 * \param name Name to append.
 * \param capacity Size of `list` in bytes including the null terminator.
 * \return `true` on success, `false` if the list is full.
 */
static bool addNameToList(char* list, const char* name, size_t capacity) {
    const size_t currentLen = strlen(list);
    const size_t nameLen = strlen(name);

    if (currentLen + nameLen + 2 >= capacity) {
        return false;
    }

    size_t writePos = currentLen;
    if (currentLen > 0) {
        list[writePos++] = ',';
    }
    memcpy(list + writePos, name, nameLen + 1);
    return true;
}

/**
 * \brief Enables or disables a module by updating persisted disabled list.
 * \param index Module index.
 * \param enabled Desired enabled state.
 */
void ModuleRegistry::setModuleEnabled(uint8_t index, bool enabled) {
    if (index >= count_) return;

    const char* name = modules_[index]->getName();
    const bool currentlyEnabled = isModuleEnabledByName(name);

    if (enabled == currentlyEnabled) return;  // No change needed

    if (enabled) {
        removeNameFromList(disabledModules_, name,
                           disabledModules_, sizeof(disabledModules_));
    } else {
        if (!addNameToList(disabledModules_, name, sizeof(disabledModules_))) {
            LOG_W(TAG, "Disabled list full, cannot add '%s'", name);
            return;
        }
    }

    saveDisabledList();
}

/**
 * \brief Toggles enabled state for a module.
 * \param index Module index.
 * \return New enabled state after toggle.
 */
bool ModuleRegistry::toggleModuleEnabled(uint8_t index) {
    if (index >= count_) return false;

    bool wasEnabled = isModuleEnabled(index);
    setModuleEnabled(index, !wasEnabled);
    return !wasEnabled;  // Return new state
}

/**
 * \brief Reports whether a module currently has a slot-validation error.
 * \param index Module index.
 * \return `true` if module has recorded slot error.
 */
bool ModuleRegistry::hasModuleSlotError(uint8_t index) const {
    if (index >= count_) return false;
    return moduleErrors_[index].hasError;
}

/**
 * \brief Returns stored slot-error message for module index.
 * \param index Module index.
 * \return Error message pointer or `nullptr` when no error is set.
 */
const char* ModuleRegistry::getModuleSlotError(uint8_t index) const {
    if (index >= count_) return nullptr;
    return moduleErrors_[index].hasError ? moduleErrors_[index].message : nullptr;
}

/**
 * \brief Sets module error state and message.
 * \param index Module index.
 * \param message Error message text.
 */
void ModuleRegistry::setModuleError(uint8_t index, const char* message) {
    if (index >= MAX_MODULES) return;
    moduleErrors_[index].hasError = true;
    if (message) {
        strncpy(moduleErrors_[index].message, message, sizeof(moduleErrors_[index].message) - 1);
        moduleErrors_[index].message[sizeof(moduleErrors_[index].message) - 1] = '\0';
    } else {
        moduleErrors_[index].message[0] = '\0';
    }
}

/**
 * \brief Clears module error state for index.
 * \param index Module index.
 */
void ModuleRegistry::clearModuleError(uint8_t index) {
    if (index >= MAX_MODULES) return;
    moduleErrors_[index].hasError = false;
    moduleErrors_[index].message[0] = '\0';
}

/**
 * \brief Records and publishes an operational module error by module name.
 * \param name Module name.
 * \param message Error message text.
 */
void ModuleRegistry::reportModuleError(const char* name, const char* message) {
    if (!name) return;

    // Find module by name
    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(modules_[i]->getName(), name) == 0) {
            // Stop the module if it's running
            if (modules_[i]->getState() == ServiceState::STARTED) {
                modules_[i]->stop();
                LOG_W(TAG, "Module '%s' stopped due to error", name);
            }
            setModuleError(i, message);
            LOG_E(TAG, "Module '%s' error: %s", name, message ? message : "(null)");

            // Publish error event for UI notification
            Event evt;
            evt.type = EventType::MODULE_ERROR;
            evt.data.value = i;
            EventBus::instance().publish(evt);
            return;
        }
    }
    LOG_W(TAG, "reportModuleError: module '%s' not found", name);
}

/**
 * \brief Clears stored module error by module name.
 * \param name Module name.
 */
void ModuleRegistry::clearModuleErrorByName(const char* name) {
    if (!name) return;

    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(modules_[i]->getName(), name) == 0) {
            clearModuleError(i);
            return;
        }
    }
}

/**
 * \brief Attempts to recover a failed module by re-initializing and restarting it.
 * \param index Module index.
 * \return `true` if retry succeeded.
 */
bool ModuleRegistry::retryModule(uint8_t index) {
    if (index >= count_) return false;

    IModule* module = modules_[index];
    if (!module) return false;

    const char* name = module->getName();
    LOG_I(TAG, "Retrying module '%s'...", name);

    // Clear the error first
    clearModuleError(index);

    // Try to re-initialize if needed
    ServiceState state = module->getState();
    if (state == ServiceState::UNINITIALIZED) {
        if (!module->init()) {
            reportModuleError(name, "Init failed on retry");
            return false;
        }
    }

    // Try to start
    if (!module->start()) {
        reportModuleError(name, "Start failed on retry");
        return false;
    }

    LOG_I(TAG, "Module '%s' retry successful", name);
    return true;
}

/**
 * \brief Slot validation helpers.
 */

/**
 * \brief Builds standardized slot-validation error text.
 * \param buffer Output buffer for formatted message.
 * \param bufSize Output buffer size.
 * \param errorType Error category text.
 * \param mapName Slot map name.
 */
void ModuleRegistry::buildSlotErrorMessage(char* buffer, size_t bufSize,
                                           const char* errorType, const char* mapName) {
    snprintf(buffer, bufSize, "%s for %s", errorType, mapName);
}

/**
 * \brief Validates global slot map state before per-module range checks.
 * \param moduleName Name used for reporting validation errors.
 * \return `true` if slot map is valid.
 */
bool ModuleRegistry::validateSlotMap(const char* moduleName) {
    const auto& slotMap = TropicSlotMap::instance();
    if (!slotMap.isValid()) {
        const char* errMsg = slotMap.errorMessage();
        reportModuleError(moduleName, errMsg ? errMsg : "slot map invalid");
        return false;
    }
    return true;
}

/**
 * \brief Validates required ECC slot range and applies it to module range.
 * \param mapName Slot map logical name.
 * \param moduleName Module name used for error reporting.
 * \param minSlots Minimum required ECC slots.
 * \param range In/out slot range result.
 * \param moduleId In/out resolved module ID.
 * \return `true` if ECC requirements are satisfied.
 */
bool ModuleRegistry::validateEccRange(const char* mapName, const char* moduleName,
                                      uint16_t minSlots, IModule::SlotRange& range,
                                      uint8_t& moduleId) {
    const auto& slotMap = TropicSlotMap::instance();
    TropicSlotMap::SlotRange ecc = {};

    if (!slotMap.getRangeByName(mapName, TropicSlotMap::SlotType::ECC, &ecc)) {
        char msg[96];
        buildSlotErrorMessage(msg, sizeof(msg), "missing ECC slot map", mapName);
        reportModuleError(moduleName, msg);
        return false;
    }

    uint16_t count = static_cast<uint16_t>(ecc.end - ecc.start + 1);
    if (count < minSlots) {
        char msg[96];
        buildSlotErrorMessage(msg, sizeof(msg), "not enough ECC slots", mapName);
        reportModuleError(moduleName, msg);
        return false;
    }

    range.hasEcc = true;
    range.eccStart = static_cast<uint8_t>(ecc.start);
    range.eccEnd = static_cast<uint8_t>(ecc.end);
    moduleId = ecc.moduleId;
    return true;
}

/**
 * \brief Validates required RMEM slot range and applies it to module range.
 * \param mapName Slot map logical name.
 * \param moduleName Module name used for error reporting.
 * \param minSlots Minimum required RMEM slots.
 * \param range In/out slot range result.
 * \param moduleId In/out resolved module ID.
 * \return `true` if RMEM requirements are satisfied.
 */
bool ModuleRegistry::validateRmemRange(const char* mapName, const char* moduleName,
                                       uint16_t minSlots, IModule::SlotRange& range,
                                       uint8_t& moduleId) {
    const auto& slotMap = TropicSlotMap::instance();
    TropicSlotMap::SlotRange rmem = {};

    if (!slotMap.getRangeByName(mapName, TropicSlotMap::SlotType::RMEM, &rmem)) {
        char msg[96];
        buildSlotErrorMessage(msg, sizeof(msg), "missing RMEM slot map", mapName);
        reportModuleError(moduleName, msg);
        return false;
    }

    uint16_t count = static_cast<uint16_t>(rmem.end - rmem.start + 1);
    if (count < minSlots) {
        char msg[96];
        buildSlotErrorMessage(msg, sizeof(msg), "not enough RMEM slots", mapName);
        reportModuleError(moduleName, msg);
        return false;
    }

    // Check for module ID mismatch (ECC and RMEM must belong to same module)
    if (moduleId != 0 && moduleId != rmem.moduleId) {
        char msg[96];
        buildSlotErrorMessage(msg, sizeof(msg), "module id mismatch", mapName);
        reportModuleError(moduleName, msg);
        return false;
    }

    range.hasRmem = true;
    range.rmemStart = rmem.start;
    range.rmemEnd = rmem.end;
    moduleId = rmem.moduleId;
    return true;
}

/**
 * \brief Slot request application orchestrator.
 */

/**
 * \brief Validates and applies slot request declared by a module.
 * \param module Module instance.
 * \param index Module index in registry.
 * \return `true` if request validation and assignment succeeded.
 */
bool ModuleRegistry::applySlotRequest(IModule* module, uint8_t index) {
    if (!module) return false;

    const char* moduleName = module->getName();

    // Step 1: Validate slot map is initialized and valid
    if (!validateSlotMap(moduleName)) {
        return false;
    }

    // Step 2: Get slot request from module
    IModule::SlotRequest req = module->getSlotRequest();
    const char* mapName = (req.mapName && req.mapName[0] != '\0') ? req.mapName : moduleName;

    // No slots required - nothing to validate
    if (!mapName || (req.minEccSlots == 0 && req.minRmemSlots == 0)) {
        return true;
    }

    IModule::SlotRange range = {};
    uint8_t moduleId = 0;

    // Step 3: Validate ECC slot range if required
    if (req.minEccSlots > 0) {
        if (!validateEccRange(mapName, moduleName, req.minEccSlots, range, moduleId)) {
            return false;
        }
    }

    // Step 4: Validate RMEM slot range if required
    if (req.minRmemSlots > 0) {
        if (!validateRmemRange(mapName, moduleName, req.minRmemSlots, range, moduleId)) {
            return false;
        }
    }

    // Step 5: Apply validated slot range to module
    range.moduleId = moduleId;
    module->setSlotRange(range);
    return true;
}

} // namespace cdc::core
