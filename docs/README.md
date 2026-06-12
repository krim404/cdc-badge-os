# CDC Badge OS Documentation

Documentation index for the CDC Badge v1.0/v1.1 hardware security key firmware.

For the feature/module list, hardware overview, build/flash commands, feature
flags, and getting started, see the [project README](../README.md).

Authoritative facts live in code, not in these docs:

- TROPIC01 slot allocation: `main/tropic_slot_map.h`
- Plugin host API surface: `components/plugin_manager/include/plugin_manager/host_api.h`
- Feature flags: `components/cdc_core/include/cdc_core/feature_flags.h`

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Native modules (mod_fido2, mod_2fa, mod_password, mod_gpg, …)   │
│  + WASM plugins (PluginManager + WAMR sandbox)                   │
├─────────────────────────────────────────────────────────────────┤
│  OS UI (cdc_os_ui): Lock Screen │ Settings │ WiFi/BLE Menus      │
├─────────────────────────────────────────────────────────────────┤
│  UI Framework (cdc_ui + cdc_views): ViewStack │ ListView │ …     │
├─────────────────────────────────────────────────────────────────┤
│  Core Services (cdc_core): ServiceRegistry │ EventBus │ …        │
├─────────────────────────────────────────────────────────────────┤
│  HAL (cdc_hal): Display │ Keypad │ SecureElement │ Power │ …     │
├─────────────────────────────────────────────────────────────────┤
│  ESP-IDF / FreeRTOS                                              │
└─────────────────────────────────────────────────────────────────┘
```

## Guides

### User

- [UI Flows](UI_FLOWS.md) - on-device navigation and interface reference
- [Serial Commands](SERIAL_COMMANDS.md) - USB serial command reference

### Developer

- [Module Development](MODULE_DEVELOPMENT.md) - native modules (compiled in)
- [Plugin Development](PLUGIN_DEVELOPMENT.md) - sandboxed WASM plugins (host API)
- [Security Hardening](SECURITY.md) - security posture and 1.0 hardening roadmap
- [GPG Implementation](GPG.md) - OpenPGP smartcard details

### Protocol Specifications

- [BLE vCard Protocol](ble_vcard_protocol.md) - badge-to-badge contact exchange
- [GPG Cross-Signing](CROSS_SIGNING.md) - badge-to-badge key signing

## Contributing

- All code and documentation in English
- Follow existing patterns in the codebase
- Use `cdc_log` for logging (never `ESP_LOG` directly)
- See [Module Development](MODULE_DEVELOPMENT.md) for architecture guidelines

## License

See repository root for license information.
