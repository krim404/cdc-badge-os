---
title: What plugins can do
description: The host capability families available to CDC Badge plugins, plus the GPIO and I2C safety policy that hard-blocks reserved pins.
sidebar:
  order: 4
---

A plugin can only do what its manifest requests and what the firmware grants. This page summarises the host capability families and the hardware-safety policy at a high level. For the full per-function contract, see the [host API reference](/dev/host-api/).

## Capability families

Plugins reach the firmware through `host_*` functions grouped into families. The families exposed by the firmware include:

| Area | Host API family | What it covers |
| --- | --- | --- |
| Storage (key/value) | `host_nvs_*` | Persistent key/value storage in a plugin-private NVS namespace. |
| Storage (files) | `host_fs_*` | Sandboxed files on the FAT partition, confined to the plugin's own folder. |
| Secure metadata | `host_rmem_*` | Named slots in the secure element's R-Memory. |
| Networking | `host_wifi_*`, `host_http_*`, `host_socket_*` | WiFi control, HTTP requests, raw sockets. |
| Bluetooth | `host_ble_*` | BLE GATT services and characteristics via the controller. |
| Badge-to-badge messaging | `host_msg_*` | MIME-typed message transfer between badges. |
| Secure element | `host_se_*` | Chip id and firmware version of the TROPIC01 secure element. |
| Crypto | `host_aes_*`, `host_base32_*`, `host_base64_*` | AES-GCM, Base32, Base64 helpers. |
| UI | `host_ui_*`, `host_view_*`, `host_display_*`, `host_i18n_*`, `host_lockscreen_*` | Views, list and canvas drawing, localized strings, lock-screen quick actions. |
| Input and events | `host_event_*` | Bus and key events. |
| GPIO and buses | `host_gpio_*`, `host_adc_*`, `host_pwm_*`, `host_i2c_*` | Digital IO, analog read, PWM, I2C, on whitelisted pins only. |
| Pixel strip | `host_pixel_*` | Addressable LED strip output. |
| Power | `host_power_*` | Power and sleep state. |
| Logging | `host_log_*` | Serial log output. |

Each sensitive area is also a manifest capability flag (for example `wifi`, `ble`, `http`, `socket`, `pixel_strip`, `usb_cdc`, `display_lowlevel`, `sao`, `grove`, `vfat`) or a resource request (`gpio_pins`, `pwm_pins`, `adc_pins`, `i2c_bus`, `rmem`, `ecc`, `ble_service_uuids`, `message_types`, `nvs_namespace`). The firmware validates these before the plugin runs.

## Storage rules

- File access (`vfat`) is confined host-side to the plugin's own folder, `/vfat/data/<id>/`. A plugin cannot read another plugin's files or escape that directory.
- NVS use requires an `nvs_namespace` that starts with `plg_` or `plugin_`, uses only `[a-z0-9_]`, and is at most 15 characters.
- Requesting R-Memory slots (`rmem`) requires a valid `nvs_namespace` to be declared as well.

## BLE and messaging rules

- Each `ble_service_uuid` must be a 128-bit lowercase, dashed UUID.
- Declaring `message_types` (MIME types) implies the plugin handles badge-to-badge messages; sending also requires the `ble` capability.

## GPIO, PWM, ADC, and I2C safety policy

GPIO, PWM, and ADC pin requests are checked against a single shared allow and block list (the same list used by both manifest validation and the raw GPIO serial shell), so a plugin cannot reach an unsafe pin by either route.

**Hard-blocked pins** (reserved by firmware hardware, never grantable):

```text
0, 1, 8, 10, 11, 12, 13, 17, 18, 19, 20, 21,
26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
39, 41, 42, 45, 46, 47, 48
```

These cover the display SPI bus, the TROPIC01 secure element, the charger and IO expander, USB, and the octal PSRAM / flash data lines.

**Plugin-usable pins** (whitelist):

```text
2, 3, 4, 5, 6, 7, 9, 14, 15, 16, 38, 40, 43, 44
```

These are the Grove, SAO, and 40-pin header pins.

For I2C, **bus 0 is reserved** for internal hardware (the charger and IO expander) and is always rejected. Plugins use the expansion bus, I2C bus 1.

:::caution
Two whitelist pins carry a secondary role. Pin 3 is an ESP32-S3 strapping pin (sampled only at reset, where it selects the JTAG signal source) and is also wired as Grove SIG1. Pin 40 is the JTAG TDO line; on a default badge JTAG runs over the built-in USB Serial/JTAG, so the pin is free, and it only matters if pin-based JTAG or an external debugger is in use. Both stay on the whitelist; use them with care on hardware that relies on those functions.
:::

## Compatibility gate

A plugin's manifest declares the host API level it targets. The firmware loads it only if the major level matches and the minor level is not newer than the firmware provides. This firmware provides host API level **0.7**.

For the complete function signatures, error codes, and level constants, see the [host API reference](/dev/host-api/) and the [plugin SDK](/dev/plugin-sdk/).
