---
title: Developing a module
description: The self-contained module pattern, the single registration point, the IModule/ModuleBase lifecycle, and where views, i18n and NVS belong.
sidebar:
  order: 3
---

A module is a self-contained feature (2FA, FIDO2, password vault, vCard, ...)
that lives entirely under `components/mod_<name>/`. The firmware core never
references a module: modules register themselves at boot and contribute menu
items, views, i18n strings and serial commands from the inside. Deleting a
module folder and removing its name from one CMake list is enough to remove it
completely.

This page uses `mod_sao` as the worked example because it is the smallest
complete module in the tree.

## What a module is

The module contract is the `IModule` interface
(`components/cdc_core/include/cdc_core/IModule.h:55`). `IModule` extends
`IService` (`components/cdc_core/include/cdc_core/IService.h:27`), which defines
the lifecycle every service shares:

| Method | Purpose | Source |
| --- | --- | --- |
| `bool init()` | One-time initialization, called once during boot | `IService.h:35` |
| `bool start()` | Begin operation; callable after `init()` or `stop()` | `IService.h:41` |
| `void stop()` | Reversible stop/cleanup | `IService.h:46` |
| `ServiceState getState() const` | Current lifecycle state | `IService.h:51` |
| `const char* getName() const` | Name for logging and lookup | `IService.h:56` |

The lifecycle states are `UNINITIALIZED`, `INITIALIZED`, `STARTED`, `STOPPED`,
`ERROR` (`IService.h:10`).

`IModule` adds module-specific virtuals on top. `getVersion()` is the only
extra pure-virtual (`IModule.h:77`); the rest are optional overrides with
default no-op or empty implementations:

| Method | Default | Source |
| --- | --- | --- |
| `const char* getVersion() const` | pure virtual (required) | `IModule.h:77` |
| `uint8_t getMenuItems(ModuleMenuItem*, uint8_t)` | returns 0 | `IModule.h:120` |
| `ui::IView* getEntryView()` | returns `nullptr` | `IModule.h:129` |
| `uint8_t getLockScreenContextItems(LockScreenContextItem*, uint8_t)` | returns 0 | `IModule.h:137` |
| `void onUnlock()` / `void onLock()` | no-op | `IModule.h:145`, `IModule.h:150` |
| `void onUsbConnect()` / `void onUsbDisconnect()` | no-op | `IModule.h:155`, `IModule.h:160` |
| `void onTick(uint32_t nowMs)` | no-op | `IModule.h:166` |
| `bool exportBackup(cJSON*)` / `BackupResult importBackup(const cJSON*)` | no-op | `IModule.h:100`, `IModule.h:112` |
| `void setSlotRange(const SlotRange&)` / `SlotRequest getSlotRequest() const` | no-op / empty | `IModule.h:172`, `IModule.h:178` |

The `onUnlock`, `onLock`, `onUsbConnect`, `onUsbDisconnect` and `onTick`
callbacks are driven by the registry, which fans the corresponding events out
to every registered module (`ModuleRegistry.h:140-144`). `onTick` is dispatched
once per main-loop iteration (`main/main.cpp:581`).

## ModuleBase: lifecycle boilerplate

Most modules do not implement the state-machine plumbing themselves.
`ModuleBase` (`components/cdc_core/include/cdc_core/ModuleBase.h:20`) is a
non-templated base that stores `state_` and `name_`, implements `getName()`,
`getState()`, and provides standard `start()` / `stop()` transitions:

- `ModuleBase::start()` only succeeds from `INITIALIZED` or `STOPPED` and moves
  to `STARTED` (`ModuleBase.h:49`).
- `ModuleBase::stop()` moves to `STOPPED` (`ModuleBase.h:61`).

A `ModuleBase` subclass implements `init()` (initialization is always
module-specific) and either inherits `start()`/`stop()` unchanged or overrides
them, calling the base for the state transition while adding its own work
(`ModuleBase.h:8-18`).

:::note[mod_sao is a ModuleBase subclass]
The worked example `mod_sao` derives from `core::ModuleBase`
(`components/mod_sao/include/mod_sao/SaoModule.h`) and passes its name to the
base constructor. It inherits `getName()`, `getState()` and `stop()`, implements
`init()` and `getVersion()`, and overrides `start()` to call `ModuleBase::start()`
for the state transition before bringing up its hardware
(`components/mod_sao/src/SaoModule.cpp`).
:::

## The single registration point

Adding a module to the build is one edit: append the component name to the
`MODULES` list in `main/CMakeLists.txt:8`. That list does three things:

1. It is spliced into the `main` component's `REQUIRES` so the module links
   (`main/CMakeLists.txt:30`).
2. CMake generates `modules_init.gen.h` at configure time, emitting an
   `extern "C" void <module>_register();` declaration plus a call inside
   `modules_register_all()` for every entry (`main/CMakeLists.txt:33-49`).
3. Boot calls `modules_register_all()` followed by
   `ModuleRegistry::runAllInitializers()` (`main/main.cpp:520-523`).

Each module must therefore export exactly one C-linkage entry point named
`mod_<name>_register`. For the example:

```cpp
extern "C" void mod_sao_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_sao::SaoModule::instance();
        module.init();
    });
}
```

(`components/mod_sao/src/SaoModule.cpp:80-85`)

The entry point does not initialize the module directly. It registers a
deferred initializer with the registry (`ModuleRegistry.h:42`,
`ModuleRegistry.cpp:53`). All initializers run later, after the system is ready
(NVS, I18n, TropicStorage), from `runAllInitializers()`
(`ModuleRegistry.cpp:67-119`).

:::note[Module init runs after TropicStorage]
Modules read slot-map metadata during their `init()`, so `runAllInitializers()`
requires `TropicStorage` to be `STARTED` and aborts module init otherwise
(`ModuleRegistry.cpp:68-76`). The boot sequence brings TropicStorage up in
`initSystemServices()` before `initModules()` (`main/main.cpp:624`,
`main/main.cpp:631`). This is init ordering, not a security gate: a secure
element that fails to come up is handled one layer down, where the HAL locks the
device instead of letting boot continue.
:::

## Boot order: init, then start

`runAllInitializers()` only calls each module's `init()` (via the registered
initializer). The registry then decides what starts:

1. Run every initializer, which calls `init()` on its module
   (`ModuleRegistry.cpp:80-84`).
2. Verify every registered module has an entry in `module_defaults.h`; a missing
   entry is logged as an error (`ModuleRegistry.cpp:88-93`).
3. Load the persisted disabled-module list from NVS
   (`ModuleRegistry.cpp:96`).
4. Stop any disabled module that started itself during `init()`
   (`ModuleRegistry.cpp:102-107`).
5. Call `startModule(i)` for exactly the enabled set
   (`ModuleRegistry.cpp:108-112`).

Inside `init()`, a module registers itself with the registry and reports
`INITIALIZED`. The example:

```cpp
bool SaoModule::init() {
    LOG_I(TAG, "Initializing SAO module");
    registerStrings();
    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}
```

(`components/mod_sao/src/SaoModule.cpp:35-41`)

`registerModule()` stores the instance, applies its slot request and logs the
registration (`ModuleRegistry.cpp:126-150`). Registration is rejected on a
duplicate name (`ModuleRegistry.cpp:138-143`).

## Default-enabled map

Every module name in `MODULES` must also appear in the compile-time
default-enabled map, `main/module_defaults.h:15`. Each `X("module_name",
default_enabled)` row gives the factory-default activation state for a fresh
install with no NVS toggle yet:

```c
#define MODULE_DEFAULT_MAP(X) \
    X("mod_2fa",        true)  \
    ...
    X("mod_usbhid",     false) \
    X("mod_otphid",     false) \
    X("mod_vfat",       true)
```

(`main/module_defaults.h:15-27`)

The name string must match `IModule::getName()`, which also matches the
`MODULES` entry (`main/module_defaults.h:9-10`). User toggles persist
independently in NVS and do not change this map
(`main/module_defaults.h:5-6`). A registered module missing from the map is a
build misconfiguration and is logged at error level
(`ModuleRegistry.cpp:88-93`).

## What belongs inside the module

Everything a module needs lives in its own component. Nothing module-specific
goes into `cdc_*` core.

### Menu items and views

A module surfaces itself in the UI by returning `ModuleMenuItem` records from
`getMenuItems()`. Each item carries a label, a sort priority, a view factory or
action callback, and a `MenuLocation` (`IModule.h:29-37`). The available
locations are `MAIN_MENU`, `TOOLS_MENU`, `SETTINGS_MENU`, `BLUETOOTH_MENU`,
`WIFI_MENU`, `EXPERT_MENU` (`IModule.h:17-24`). The registry collects items per
location and sorts them by priority (`ModuleRegistry.h:117`). The example
contributes one item under Tools:

```cpp
uint8_t SaoModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;
    items[0] = {
        ui::tr("mod_sao.title"),
        120,
        getInfoView,
        nullptr,
        getName(),
        core::MenuLocation::TOOLS_MENU,
        nullptr
    };
    return 1;
}
```

(`components/mod_sao/src/SaoModule.cpp:59-71`)

The view factory returns an `IView*` (`IModule.h:32`). The example keeps its
view object static inside the module and fills it on demand
(`SaoModule.cpp:21-28`). Module-specific views live in the module, not in
`cdc_views`; `cdc_views` is only for reusable views (see
[UI framework & display](/dev/ui-framework/)).

### I18n strings

A module ships its English strings as a `static constexpr I18nEntry[]` table and
registers it in `init()` via
`I18n::instance().registerEnglishTable(table, count)`
(`components/cdc_ui/include/cdc_ui/I18n.h:106`). The entries are stored by
pointer, so the table must stay alive for the firmware lifetime; rodata
literals are fine (`I18n.h:96-106`). The example:

```cpp
constexpr ui::I18nEntry kStrings[] = {
    {"mod_sao.title", "SAO"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}
```

(`components/mod_sao/src/SaoModule.cpp:13-19`)

Module keys use the `mod_<name>.*` convention (`I18n.h:22`). At display time
text is looked up with `ui::tr("mod_sao.title")` (`I18n.h:208`). Non-English
translations live in the per-language overlay files, not in the module (see the
project i18n guidelines).

### NVS namespace

Modules persist state under a per-module NVS namespace formed from the prefix
`"mod_"` plus the module name (`ModuleRegistry.h:55`,
`ModuleRegistry.cpp:483`). When a module is removed from the build, the registry
detects its name is no longer registered and erases that namespace on the next
boot (`ModuleRegistry.cpp:441-495`), so a deleted module leaves no orphaned NVS
data behind.

## CMakeLists for a module

A module's `CMakeLists.txt` is a normal `idf_component_register` that lists its
sources, its `include` dir and its `REQUIRES`. The example:

```cmake
idf_component_register(
    SRCS
        "src/SaoModule.cpp"
        "src/sao.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
)
```

(`components/mod_sao/CMakeLists.txt:3-15`)

:::caution[BLE-using modules]
This is the BLE-specific case of the general rule that modules reach hardware
only through HAL interfaces, never the driver (see
[Architecture](/dev/architecture/)). BLE is singled out because NimBLE is the
easiest HAL to bypass and the most damaging to bypass: its GAP event handler
must be singular, so a second handler causes connection-state conflicts. A
module that uses Bluetooth must NOT add `bt` to its `REQUIRES` and must not touch
NimBLE directly; it uses `IBluetoothController` only.
:::

## Checklist for a new module

1. Create `components/mod_<name>/` with `CMakeLists.txt`,
   `include/mod_<name>/<Name>Module.h` and `src/<Name>Module.cpp`.
2. Derive the module class from `ModuleBase` (or `IModule`), implement `init()`
   and `getVersion()`, override the lifecycle callbacks you need.
3. Export `extern "C" void mod_<name>_register()` that registers a deferred
   initializer calling your module's `init()`
   (`SaoModule.cpp:80-85`).
4. In `init()`: register your English i18n table, call
   `ModuleRegistry::instance().registerModule(this)`, set state to
   `INITIALIZED` (`SaoModule.cpp:35-41`).
5. Add `mod_<name>` to `MODULES` in `main/CMakeLists.txt:8`.
6. Add `X("mod_<name>", <true|false>)` to `MODULE_DEFAULT_MAP` in
   `main/module_defaults.h:15`.
7. Keep all views, i18n strings, NVS access and helpers inside the component.
