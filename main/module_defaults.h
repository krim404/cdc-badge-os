#pragma once

// Compile-time module default-enabled map (edit before build)
//
// Lists every module with its factory-default activation state for a fresh
// install (no NVS entry yet). User toggles persist independently in NVS and
// are not affected by this map.
//
// Module names must match the module runtime name (IModule::getName()), which
// also matches the entry in main/CMakeLists.txt MODULES.

namespace cdc::module_defaults {

// X("module_name", default_enabled)
#define MODULE_DEFAULT_MAP(X) \
    X("mod_2fa",        true)  \
    X("mod_fido2",      true)  \
    X("mod_password",   true)  \
    X("mod_gpg",        true)  \
    X("mod_sao",        true)  \
    X("mod_vcard",      true)  \
    X("mod_ble_serial", true)  \
    X("mod_nvsedit",    true)  \
    X("mod_blehid",     true)  \
    X("mod_usbhid",     false) \
    X("mod_otphid",     false) \
    X("mod_vfat",       true)  \
    X("mod_msc",        false)

} // namespace cdc::module_defaults
