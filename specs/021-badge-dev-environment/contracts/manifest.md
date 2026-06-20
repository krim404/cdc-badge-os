# Contract: Plugin Manifest (`meta.json`)

The manifest schema is **owned upstream** in `vendor/cdc-badge-plugins/docs/manifest_schema.md` and validated by `vendor/cdc-badge-plugins/tools/validate_manifest.py`. This repository references that schema and does not redefine it (single source of truth).

The `badge` CLI and the `plugins/starter/` template enforce the required surface before upload:

| Field | Rule | Failure mode if wrong |
|-------|------|-----------------------|
| `id` | `[a-z][a-z0-9_]{1,31}`, unique; filename stem | upload/rename rejected locally |
| `version` | SemVer `MAJOR.MINOR.PATCH` | manifest validation error |
| `host_api_level_min` | `MAJOR.MINOR` | badge refuses load if higher minor / other major than firmware exposes |
| `linear_memory_kb` | integer 16–1024 | validation error |
| `capabilities` | gates (`wifi`/`ble`/`http`/`socket`/`vfat`/…), behavior flags (`background`/`autoload`/`prevent_sleep`), named resources (`rmem`/`ecc`/`gpio_pins`/`pwm_pins`/`adc_pins`/`i2c_bus`/`message_types`), `nvs_namespace` | badge rejects a host call whose capability is undeclared; GPIO on the hard-block list is rejected |
| `i18n` | `default_language`, `i18n.meta.{name,description}`, `i18n.strings` (English fallbacks) | missing names fail validation |
| `prerequisites` (optional) | known keys (`wifi_connected`, `time_synced`, `battery_min`, `unlocked`, …) with `on_fail` ∈ {`abort`,`warn`,`callback`} | unknown prerequisite ignored/validation-flagged |

Validation runs via the upstream `validate_manifest.py`; the skill explains rejection reasons surfaced over serial (size limit, capability, host-API level, auth).
