# Module Development Guide

How to create custom **native** modules for CDC Badge OS - C++ features
compiled into the firmware and added by reflashing.

> **Related:** [Architecture Overview](README.md#architecture) | [Serial Commands](SERIAL_COMMANDS.md) | [Plugin Development](PLUGIN_DEVELOPMENT.md)

> **Native module vs plugin:** this guide covers native modules, which are
> compiled in and require a reflash. For sandboxed **WASM plugins** loaded at
> runtime (no reflash) via the host API, see
> [PLUGIN_DEVELOPMENT.md](PLUGIN_DEVELOPMENT.md).

## Overview

Modules are self-contained features (TOTP, FIDO2, Password Vault, etc.) that can be added or removed without modifying core code. Each module provides:

- **Menu items** - Entries in the main menu or tools menu
- **Serial commands** - Commands for the serial console
- **Views** - UI screens for the E-Paper display
- **Storage** - Optional TROPIC01 secure element slots

## Module Structure

```
components/<module_name>/
├── CMakeLists.txt
├── include/<module_name>/
│   ├── <Module>Module.h
│   └── (additional headers)
└── src/
    ├── <Module>Module.cpp
    └── (additional sources)
```

## Step 1: Create the Component

Create the directory structure:

```bash
mkdir -p components/mod_example/include/mod_example
mkdir -p components/mod_example/src
```

## Step 2: CMakeLists.txt

Create `components/mod_example/CMakeLists.txt`:

```cmake
# Example Module

idf_component_register(
    SRCS
        "src/ExampleModule.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        serial_cmd
        cdc_log
)
```

Add dependencies as needed:
- `mbedtls` - for cryptography
- `nvs_flash` - for NVS storage
- `freertos` - for FreeRTOS primitives

### What `cdc_views` provides (and what it doesn't)

`cdc_views` is the public framework-view layer. Modules use it for generic UI
building blocks: `ListView`, `ConfirmView`, `InfoView`, `ToastView`,
`QRCodeView`, `T9InputView`, `ContextMenuView`. These are
intentionally module-facing and reusable.

`cdc_os_ui` is the OS-only layer (LockScreen, Settings, Wifi/Bluetooth menus,
HardwareInfo, ExpertMenu). Modules **must not** depend on `cdc_os_ui`.

Module-specific views (e.g. a vCard wizard, a FIDO2 credential-management
screen) live **inside the module** and may compose `cdc_views` primitives,
but they should not be exported into `cdc_views` itself.

## Step 3: Module Header

Create `components/mod_example/include/mod_example/ExampleModule.h`:

```cpp
#pragma once

#include "cdc_core/ModuleBase.h"

namespace cdc::mod_example {

class ExampleModule : public core::ModuleBase {
public:
    // Required: lifecycle. start()/stop() are inherited from ModuleBase;
    // override stop() only if cleanup work is needed.
    bool init() override;

    // Module identification (getName()/getState() come from ModuleBase)
    const char* getVersion() const override { return "1.0"; }

    // Optional: Menu items
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;

    // Optional: TROPIC01 slot allocation
    core::IModule::SlotRequest getSlotRequest() const override;
    void setSlotRange(const core::IModule::SlotRange& range) override;

    // Optional: Event callbacks
    void onUnlock() override;
    void onLock() override;
    void onUsbConnect() override;
    void onUsbDisconnect() override;
    void onTick(uint32_t nowMs) override;

    // Singleton access
    static ExampleModule& instance();

private:
    ExampleModule() : ModuleBase("mod_example") {}
    core::IModule::SlotRange slotRange_ = {};
};

} // namespace cdc::mod_example

// C registration function (required!)
extern "C" void mod_example_register();
```

## Step 4: Module Implementation

Create `components/mod_example/src/ExampleModule.cpp`:

```cpp
#include "mod_example/ExampleModule.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ToastView.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/Console.h"
#include "cdc_log.h"

static const char* TAG = "EXAMPLE";

namespace cdc::mod_example {

// =============================================================================
// I18n Strings (Module-local English fallback table)
// =============================================================================

constexpr ui::I18nEntry kStrings[] = {
    {"mod_example.title", "Example"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

// =============================================================================
// Serial Commands
// =============================================================================

static constexpr const char* CMD_MODULE = "example";
static bool s_commandsRegistered = false;

static void cmd_example_status(const char* args) {
    (void)args;
    cdc::serial::Console::printf("Example module OK\r\n");
}

static void registerCommands() {
    if (s_commandsRegistered) return;
    auto& reg = cdc::serial::getCommandRegistry();

    reg.registerCommand({
        "EXAMPLE_STATUS",           // Command name
        "Show example status",      // Description
        cmd_example_status,         // Handler function
        CMD_MODULE,                 // Module name
        true                        // Requires authentication
    });

    s_commandsRegistered = true;
}

// =============================================================================
// UI Views
// =============================================================================

static ui::ListView s_listView;
static bool s_viewsInitialized = false;

static void onListSelect(uint16_t index, void* userData) {
    (void)index;
    (void)userData;
    ui::showToastSuccess("Selected!");
}

static ui::IView* getMainView() {
    if (!s_viewsInitialized) {
        s_listView.setOnSelect(onListSelect);
        s_viewsInitialized = true;
    }

    static ui::ListItem items[] = {
        {"Item 1", 0, false, nullptr},
        {"Item 2", 0, false, nullptr},
    };

    s_listView.init(ui::tr("mod_example.title"), items, 2);
    return &s_listView;
}

// =============================================================================
// Module Implementation
// =============================================================================

ExampleModule& ExampleModule::instance() {
    static ExampleModule inst;
    return inst;
}

bool ExampleModule::init() {
    LOG_I(TAG, "Initializing Example module");

    registerStrings();
    registerCommands();

    // Register with ModuleRegistry
    core::ModuleRegistry::instance().registerModule(this);

    // If module requires TROPIC01 slots, validate them
    if (slotRange_.hasRmem) {
        // Configure storage with slot range
        core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    }

    state_ = core::ServiceState::INITIALIZED;
    return true;
}

// start() and stop() are inherited from ModuleBase. Override stop() only when
// the module needs to release resources before the state transition.

void ExampleModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

core::IModule::SlotRequest ExampleModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minRmemSlots = 0;  // Set to >0 if you need TROPIC01 storage
    return req;
}

uint8_t ExampleModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {
        ui::tr("mod_example.title"),  // Label (translated)
        100,                        // Priority (lower = higher in list)
        getMainView,               // View factory function
        nullptr,                   // Visibility check (nullptr = always visible)
        getName(),                 // Module name (for debugging)
        core::MenuLocation::MAIN_MENU  // Where to show
    };

    return 1;
}

// Optional event callbacks
void ExampleModule::onUnlock() {
    LOG_D(TAG, "Device unlocked");
}

// NOTE: onUnlock() is dispatched right after a successful PIN unlock
// (see AppUi onPinSuccess → ModuleRegistry::dispatchUnlock()).
// Use this hook for actions that must only happen after PIN entry.

void ExampleModule::onLock() {
    LOG_D(TAG, "Device locked");
}

void ExampleModule::onUsbConnect() {
    LOG_D(TAG, "USB connected");
}

void ExampleModule::onUsbDisconnect() {
    LOG_D(TAG, "USB disconnected");
}

void ExampleModule::onTick(uint32_t nowMs) {
    (void)nowMs;
    // Called periodically for background work
}

} // namespace cdc::mod_example

// =============================================================================
// Registration Function (C linkage!)
// =============================================================================

extern "C" void mod_example_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_example::ExampleModule::instance();
        if (module.init()) {
            module.start();
        }
    });
}
```

## Step 5: Enable the Module

Add your module to `main/CMakeLists.txt`:

```cmake
set(MODULES
    mod_totp
    mod_fido2
    mod_password
    mod_gpg
    mod_example    # <-- Add here
)
```

## Step 6: Configure TROPIC01 Storage (if needed)

If your module requires secure storage in the TROPIC01 chip, you must edit the memory map.

Edit `main/tropic_slot_map.h`:

```cpp
// 1. Add a Module ID (must be unique, 0-254; 255 = UNKNOWN)
#define MODULE_ID_MOD_EXAMPLE 8

// 2. Define an R-Memory range (slots 1-511; 0 is reserved)
#define RMEM_SLOT_MOD_EXAMPLE_START <start>
#define RMEM_SLOT_MOD_EXAMPLE_END   <end>

// 3. Add to the R-Memory slot map macro
#define TROPIC_RMEM_SLOT_MAP(X) \
    ... existing entries ... \
    X("mod_example", MODULE_ID_MOD_EXAMPLE, RMEM_SLOT_MOD_EXAMPLE_START, RMEM_SLOT_MOD_EXAMPLE_END)
```

The ECC and R-Memory maps in `main/tropic_slot_map.h` are fully allocated
today (see the slot table below). A new module must carve its range out of an
existing allocation; the validator rejects overlaps and out-of-bounds ranges.
ECC slots (1-30 in use, 31 reserved for plugins) cannot be added without
shrinking another module's ECC range first.

**Important:** The module name in the slot map (`"mod_example"`) must exactly match `getName()` in your module class.

## Files to Edit Summary

| File | Purpose | When to Edit |
|------|---------|--------------|
| `main/CMakeLists.txt` | Enable module | Always |
| `main/tropic_slot_map.h` | TROPIC01 storage allocation | Only if module needs secure storage |

That's it! The build system auto-generates the registration code in `modules_init.gen.h`.

---

## How Module Loading Works

Understanding the boot sequence helps debug module issues:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         BOOT SEQUENCE                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. Hardware Init (NVS, USB, I2C, Display, Keypad, TROPIC01)       │
│                           │                                         │
│                           ▼                                         │
│  2. modules_register_all()  ◄── Auto-generated from CMakeLists.txt │
│     Calls each <module>_register() function                         │
│     Each module registers an initializer lambda                     │
│                           │                                         │
│                           ▼                                         │
│  3. ModuleRegistry::runAllInitializers()                            │
│     For each registered initializer:                                │
│       - Calls module.init()                                         │
│       - Validates slot map (if module requests storage)             │
│       - Calls module.start()                                        │
│                           │                                         │
│                           ▼                                         │
│  4. ui_on_modules_ready()                                           │
│     Rebuilds menus with module menu items                           │
│                           │                                         │
│                           ▼                                         │
│  5. Main Loop                                                       │
│     - EventBus processing                                           │
│     - Serial command processing                                     │
│     - UI updates                                                    │
│     - ModuleRegistry::dispatchTick() to all modules                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Registration Function

The `<module>_register()` function is the entry point:

```cpp
extern "C" void mod_example_register() {
    // This runs EARLY - before system is fully ready
    // Only register an initializer, don't do heavy work here
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        // This lambda runs LATER - when system is ready
        auto& module = cdc::mod_example::ExampleModule::instance();
        if (module.init()) {
            module.start();
        }
    });
}
```

### Slot Map Validation

When a module requests TROPIC01 storage, the `ModuleRegistry` validates:

1. The slot map in `tropic_slot_map.h` has an entry for this module
2. The module name matches exactly
3. Slot ranges don't overlap with other modules
4. Slots are within valid bounds (ECC: 1-31, RMEM: 32-511)

If validation fails, the module enters ERROR state and won't start.

### Auto-Generated Code

The build system generates `main/modules_init.gen.h`:

```cpp
// Auto-generated - DO NOT EDIT
extern "C" void mod_totp_register();
extern "C" void mod_fido2_register();
extern "C" void mod_password_register();
extern "C" void mod_gpg_register();

inline void modules_register_all() {
    mod_totp_register();
    mod_fido2_register();
    mod_password_register();
    mod_gpg_register();
}
```

---

## TROPIC01 Memory Map Reference

The TROPIC01 secure element has two storage types:

### ECC Key Slots (32 slots: 0-31)

| Slot | Reserved For |
|------|--------------|
| 0 | System (Attestation Key) |
| 1-3 | mod_gpg |
| 4 | mod_ca (reserved) |
| 5-30 | mod_fido2 |
| 31 | WASM plugin pool (single reserved ECC slot, `capabilities.ecc`) |

### R-Memory Slots (512 slots: 0-511, 422 bytes payload each, 444-byte slot incl. 22-byte header)

| Slots | Reserved For |
|-------|--------------|
| 0 | System (PinManager: PIN hashes + ECDSA attestation signature) |
| 1-3 | mod_gpg (paired with ECC 1-3: SIG / DEC / AUT companion slots) |
| 4 | mod_ca (paired with ECC 4) |
| 5-31 | mod_fido2 (27 credentials; companion slots for ECC 5-30) |
| 32-131 | mod_totp (100 accounts) |
| 132-500 | mod_password (369 entries) |
| 501-511 | WASM plugin named slots (`capabilities.rmem` in manifest) |

**Rules:**
- Slot 0 is always reserved for PinManager (both ECC and RMEM)
- R-Memory slots 1-31 follow the ECC-companion convention: RMEM slot N is the
  metadata / software-wrap slot for ECC slot N and may only be used by the
  module that owns the matching ECC range.
- Module-driven allocations not tied to ECC slots use slot 32 or higher.
- Ranges must not overlap
- Module IDs must be unique (0-254, 255 = reserved)

---

## Key Concepts

### Menu Locations

```cpp
enum class MenuLocation : uint8_t {
    MAIN_MENU,       // Top-level main menu
    TOOLS_MENU,      // Under Tools submenu
    SETTINGS_MENU,   // Under Settings submenu
    BLUETOOTH_MENU,  // Under Bluetooth submenu (for BLE services)
    WIFI_MENU,       // Under WiFi submenu
    EXPERT_MENU      // Under Expert submenu (for advanced tools)
};
```

### Menu Priority

Lower priority values appear higher in the menu:
- 0-49: Core features (FIDO2, SSH)
- 50-99: Secondary features (TOTP, Password)
- 100+: Tools and utilities

### I18n (Internationalization)

Register an English fallback table once at init, then look strings up by key:

```cpp
// English fallbacks (rodata), registered in init()
constexpr ui::I18nEntry kStrings[] = {
    {"mod_name.title", "Title"},
    {"mod_name.save",  "Save"},
};
ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));

// Use strings anywhere by key
ui::tr("mod_name.title");
```

Other languages are flat `/plugins/i18n/lang_<code>.json` overlay files keyed
by the same strings; the active overlay is loaded into PSRAM at boot. To ship
a German translation, add each key to `assets/i18n/lang_de.json`. Keys must use
the `mod_<name>.` prefix and be 7-bit ASCII snake_case.

**Note:** German overlay values use real UTF-8 umlauts (`ä ö ü Ä Ö Ü ß`); the
loader converts them to CP437 for the display.

### Logging

Always use `cdc_log`, never ESP-IDF's `esp_log`:

```cpp
#include "cdc_log.h"

LOG_I(TAG, "Info message");
LOG_W(TAG, "Warning");
LOG_E(TAG, "Error");
LOG_D(TAG, "Debug");
```

### Serial Commands

```cpp
reg.registerCommand({
    "CMD_NAME",             // Command (uppercase)
    "Description",          // Help text
    handler_function,       // void handler(const char* args)
    "module_name",          // Module for HELP grouping
    true                    // Requires auth (true for sensitive commands)
});
```

### TROPIC01 Storage

If your module needs secure storage in TROPIC01:

1. Request slots in `getSlotRequest()`:
```cpp
SlotRequest req = {};
req.mapName = getName();
req.minRmemSlots = 10;  // R-Memory slots (422 bytes payload each)
req.minEccSlots = 2;    // ECC key slots (optional)
return req;
```

2. Use the assigned range in `setSlotRange()`:
```cpp
void setSlotRange(const SlotRange& range) {
    if (range.hasRmem) {
        // range.rmemStart to range.rmemEnd are your slots
        myStore.configure(range.rmemStart, range.rmemEnd);
    }
}
```

3. Configure slot allocation in `main/tropic_slot_map.h`.

### Static View Instances

Avoid dynamic allocation for views - use static instances:

```cpp
static ui::ListView s_listView;
static ui::T9InputView s_t9Input;
static MyCustomView s_customView;
```

### Memory Considerations

- Use `EXT_RAM_BSS_ATTR` for large arrays (moves to PSRAM)
- Prefer static allocation over dynamic
- Clean up resources in `stop()`

### Lock Screen Context Items

Modules can add items to the lock screen context menu (accessible without PIN):

```cpp
uint8_t MyModule::getLockScreenContextItems(LockScreenContextItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {
        []() { return "vCard QR"; },  // Dynamic label getter
        []() { showVcardQr(); },       // Callback function
        50,                             // Priority
        getName()                       // Module name (set automatically)
    };
    return 1;
}
```

### Typed Service Pattern (IKeyboardProvider)

Modules can provide or consume optional services using the typed service pattern:

**Providing a service (e.g., mod_hid provides keyboard):**

```cpp
#include "cdc_core/ServiceRegistry.h"
#include "cdc_core/IKeyboardProvider.h"

// In module init/start:
ServiceRegistry::instance().provide<IKeyboardProvider>(
    ServiceType::KEYBOARD, &myKeyboardImpl);
```

**Consuming a service (e.g., mod_totp uses keyboard for auto-type):**

```cpp
#include "cdc_core/IKeyboardProvider.h"

// When user wants to type a TOTP code:
auto* keyboard = cdc::core::getKeyboard();
if (keyboard && keyboard->isConnected()) {
    keyboard->typeString(totpCode);
}
```

**Available service types:**

| ServiceType | Interface | Description |
|-------------|-----------|-------------|
| `KEYBOARD` | `IKeyboardProvider` | BLE/USB HID keyboard for auto-type |

## Best Practices

1. **Self-contained**: All module code stays in the module directory
2. **No core modifications**: Never modify core code for module features
3. **Graceful degradation**: Handle missing slot ranges or failed init
4. **Error reporting**: Use `ModuleRegistry::reportModuleError()` for user-visible errors
5. **Static views**: Use static view instances to avoid memory fragmentation
6. **English code**: All code, comments, and docs in English

## Example Modules

Reference existing modules for patterns:

- `mod_totp` - Simple module with list view and wizard
- `mod_fido2` - Complex module with USB HID integration
- `mod_password` - Module with TROPIC01 storage
- `mod_vcard` - BLE service module with lock screen context items
- `mod_hid` - Service provider module (IKeyboardProvider)
- `mod_sao` - Minimal hardware detection module

For the addressable WS2813 strip on the Grove port, see the `grove_led`
WASM plugin in `cdc-badge-plugins/plugins/grove_led/` - that functionality
is no longer a built-in module.

## Troubleshooting

### Module not appearing in menu

1. Check `main/CMakeLists.txt` includes your module
2. Verify `<modulename>_register()` function exists with C linkage
3. Check logs for init errors

### Strings not translating

1. Ensure `registerStrings()` is called in `init()`
2. Check the key passed to `ui::tr()` matches an `I18nEntry` key exactly
3. For a non-English language, verify the key exists in `lang_<code>.json`

### Serial commands not working

1. Verify `registerCommands()` is called in `init()`
2. Check command name is UPPERCASE
3. Try `HELP` to see if command is listed
