---
title: Serial command reference
description: Complete, source-verified reference of every serial command registered by the firmware, grouped by module, with arguments and AUTH gating.
sidebar:
  order: 14
---

This is the complete reference of serial commands actually registered in the
firmware, verified against the command-registration code. For connecting and the
authentication model, see the [serial console](/power/serial-console/) page.

## How commands are registered

Each command is registered through `getCommandRegistry().registerCommand(...)`
with a `Command` descriptor (`components/serial_cmd/include/serial_cmd/ICommandRegistry.h`).
Grouped commands carry a `SubCommand` table and dispatch with
`dispatchSubCommand()` (`components/serial_cmd/include/serial_cmd/SubCommand.h`).

The descriptor's `requiresAuth` boolean is the source of truth for the "AUTH"
column below. Two rules combine at dispatch time
(`components/serial_cmd/src/CommandRegistry.cpp`):

1. With `FEATURE_SECURE_SERIAL` enabled (the default), every command except
   `PING` and `AUTH` requires a prior `AUTH <pin>` regardless of its flag.
2. The per-command `requiresAuth` flag gates the command even when secure serial
   is disabled.

Auth is checked at the **group level**: every subcommand of an AUTH-gated group
needs authentication, including read-only ones.

## Global commands

### System

| Command | Arguments | AUTH | Description |
|---------|-----------|------|-------------|
| `HELP` | - | No | List all registered commands, including subcommands |
| `PING` | - | No | Liveness check; replies `PONG` (works even during PIN lockout) |
| `STATUS` | - | No | Free heap, min free heap, uptime |
| `MEM` | - | No | Heap / internal DRAM / DMA / PSRAM usage |
| `MEMINFO` | - | No | Detailed per-region heap and FreeRTOS task watermarks |
| `CPU` | - | No | Measure aggregate CPU load over ~250 ms |
| `ERROR_LOG` | `[CLEAR]` | No | Dump the error log; `CLEAR` resets it |
| `REBOOT` | - | Yes | Restart the device |
| `BOOTLOADER` | - | Yes | Reboot into USB download (flash) mode |
| `SHIPMODE` | - | Yes | Enter ship mode (disconnect the battery for storage/shipping) |
| `PASTE` | `<text>` | Yes | Append text into the active on-device T9 input view |

### Authentication

Registered only when `FEATURE_SECURE_SERIAL` is enabled (the default).

| Command | Arguments | AUTH | Description |
|---------|-----------|------|-------------|
| `AUTH` | `<pin>` | No | Authenticate with the badge PIN; no argument logs out |
| `LOGOUT` | - | No | End the authenticated session |

### Time

| Command | Arguments | AUTH | Description |
|---------|-----------|------|-------------|
| `GET_TIME` | - | No | Print current local time |
| `GET_DATE` | - | No | Print current local date |
| `SET_TIME` | `HH:MM:SS` | No | Set the time of day |
| `SET_DATE` | `DD.MM.YYYY` or `<unix_seconds>` | No | Set the date, or full clock from a Unix timestamp |

### Display

| Command | Arguments | AUTH | Description |
|---------|-----------|------|-------------|
| `SET_NAME` | `<text>` | No | Set the lock-screen display name |
| `SET_INFO` | `<text>` | No | Set info line 1 |
| `SET_INFO2` | `<text>` | No | Set info line 2 |

### NVS (`NVS`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | `[namespace]` | List entries, optionally filtered by namespace |
| `READ` | `<ns> <key>` | Read and print a key's value |
| `DEL` | `<ns> [key]` | Delete a key, or the whole namespace if key omitted |
| `CLEAR` | `YES` | Erase all NVS (confirmation token `YES` required) |

### PIN management (`PIN`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `STATUS` | - | Show PIN retries / lockout / set state |
| `RESET` | - | Reset PIN retry counter (debug) |
| `CHANGE` | `<currentPin> <newPin>` | Change the badge PIN (4-8 digits) |
| `DURESS` | `<pin>` | Arm the self-destruct PIN (wipes the badge when entered) |
| `DURESS_CLEAR` | - | Disarm the self-destruct PIN |

### TROPIC01 secure element (`TR01`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `STATUS` | - | Show secure-element session status |
| `INFO` | - | Show chip ID and firmware versions |
| `SESSION` | - | Start or restart the SE session |
| `SLOTS` | - | Summarise ECC and R-Memory slot usage |
| `RMEM_READ` | `<slot>` | Read and hex-dump an R-Memory slot |
| `ECC_DEL` | `<slot>` | Delete an ECC key slot |
| `RMEM_DEL` | `<slot>` | Erase an R-Memory slot |
| `RESYNC` | - | Resync the SE session and cache |
| `CACHE_REBUILD` | - | Rebuild the slot cache from the chip |
| `CLEANUP` | - | Clean up mismatched slots and rebuild the cache |
| `WIPE` | `CONFIRM` | Factory-reset all SE data (confirmation token required) |

### WiFi (`WIFI`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `SCAN` | - | Scan for networks (deduplicated, sorted by RSSI) |
| `STATUS` | - | Show radio state and saved configuration |
| `ON` | `[sta\|ap\|sta_ap]` | Enable the radio (default STA, auto-reconnects if configured) |
| `OFF` | - | Disable the radio |
| `CONNECT` | `<ssid> <password>` | Connect and persist credentials |
| `TIMEOUT` | `[ms]` | Get or set the connect timeout (3000-60000 ms) |
| `FORGET` | - | Clear the saved WiFi configuration |

### Module control (`MODULE`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | - | List modules with state and slot errors |
| `ENABLE` | `<name>` | Enable a module (persistent) |
| `DISABLE` | `<name>` | Disable a module (persistent) |

## Module commands

These commands are registered by their respective modules and are present only
when the module is built in (`main/CMakeLists.txt` MODULES list).

### GPG card (`GPG`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `STATUS` | - | Show keys, fingerprints and counters |
| `GENERATE` | `<curve> <user_id>` | Generate SIG+DEC+AUT keys (curve `1`=Ed25519, `2`=P-256) |
| `EXPORT` | - | Print primary and subkey public keys as PEM |
| `RESET` | `[token]` | Two-step destructive reset of all GPG keys |
| `RECV_LIST` | - | List received cross-sign keys |
| `RECV_INFO` | `<index>` | Show a received key's details |
| `RECV_DELETE` | `<index>` | Delete a received key |
| `CROSS_SIGN` | `<index>` | Cross-sign a received key with the badge SIG subkey |
| `EXPORT_SIGNED` | `<index>` | Export a signed key as an ASCII-armored OpenPGP block |

### 2FA / OATH (`TOTP` and `CHALRESP`, AUTH-gated)

| Command | Subcommand | Arguments | Description |
|---------|------------|-----------|-------------|
| `TOTP` | `LIST` | - | List all 2FA entries |
| `TOTP` | `ADD` | `<type:totp\|hotp> <name> <secret> [issuer] [digits] [period] [algo] [counter]` | Add a TOTP or HOTP entry |
| `TOTP` | `DEL` | `<index>` | Delete an entry by index |
| `TOTP` | `GET` | `<index>` | Generate a code by index |
| `CHALRESP` | - | `<name> <hex-challenge>` | Compute an HMAC-SHA1 challenge-response for a CR entry |

### Password vault (`PASSWORD`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | - | List password entries (sorted by title) |
| `GET` | `<slot>` | Show one entry by slot |
| `ADD` | `<slot\|x> <title> <user\|x> <pw\|x> <url\|x> <totp\|-> [notes]` | Add an entry; `x` skips a field |
| `EDIT` | `<slot> <field> <value>` | Edit one field of an existing entry |
| `DEL` | `<slot>` | Delete an entry by slot |

### vCard (`VCARD`, open)

This is the one module group that is **not** AUTH-gated by its own flag (it still
requires a prior `AUTH` when secure serial is enabled).

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `SET` | `[id]` | Set own vCard, or overwrite received vCard `<id>` (multiline paste, terminate with `---` or `ABORT`) |
| `GET` | `[id]` | Show own vCard, or received vCard `<id>` |
| `LIST` | - | List received vCards as `<id>  <name>` |
| `DELETE` | `[id]` | Delete own vCard, or received vCard `<id>` |

### Encrypted backup (`BACKUP`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `EXPORT` | `<passphrase>` | Write an encrypted backup to the plugins FAT partition |
| `IMPORT` | `<passphrase>` | Restore from the on-device backup |
| `DELETE` | - | Delete the on-device backup |

### Plugin FAT shell (`VFAT`, AUTH-gated)

Operates on the plugins FAT partition with a stateful working directory (reset to
root when the serial session locks).

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | - | List the current directory |
| `PWD` | - | Print the working directory |
| `CD` | `<path>` | Change directory (`..` = up) |
| `GET` | `<file>` | Print a file's contents |
| `PUT` | `<file> <text>` | Write text (`\n` becomes a newline) |
| `RECEIVE` | `<file> <size> <crc>` | Stream a binary file into the current directory |
| `DELETE` | `<file>` | Delete a file |
| `MKDIR` | `<name>` | Create a directory |
| `RMDIR` | `<name>` | Remove an empty directory |
| `FREE` | - | Show partition usage |

### Plugin manager (`PLUGIN`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | - | List installed plugins (JSON) |
| `INFO` | `<id>` | Show manifest details for one plugin |
| `START` | `<id>` | Start a plugin |
| `STOP` | - | Stop the currently active plugin |
| `CMD` | `<id> <args>` | Forward a command string to a plugin |
| `DISABLE` | `<id>` | Disable a plugin and unload it from RAM |
| `ENABLE` | `<id>` | Enable a disabled plugin |
| `DELETE` | `<id>` | Delete a plugin's wasm + meta + lang files |
| `UPLOAD` | `<id> <size> <crc32_hex>` | Upload a `.wasm` payload (binary stream) |
| `UPLOAD_AOT` | `<id> <size> <crc32_hex>` | Upload an `.aot` payload (binary stream) |
| `UPLOAD_META` | `<id> <size> <crc32_hex>` | Upload a `.meta` payload (binary stream) |
| `UPLOAD_LANG` | `<id> <size> <crc32_hex>` | Upload a `.lang` payload (binary stream) |
| `ABORT` | - | Abort an active upload session |
| `DEBUG` | - | Toggle verbose plugin / host-API logging |

### i18n overlay (`LANG`, AUTH-gated)

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `INFO` | - | Show the active language and available overlays |
| `RELOAD` | - | Rescan and reload overlays from `/plugins/i18n/` |

## Upload streaming protocol

The `PLUGIN UPLOAD*` family and `VFAT RECEIVE` switch the console into a raw
byte-streaming mode via a byte interceptor: the badge replies `READY`, then
consumes exactly `<size>` raw bytes, computes a CRC-32 and verifies it against
the announced `<crc32_hex>` before committing the file. While streaming, normal
line parsing is fully bypassed.

## Hardware access (GPIO / ADC / I2C / SAO, AUTH-gated)

A native mini-module in `plugin_manager` exposes direct hardware poking over the
serial console (`components/plugin_manager/src/GpioSerialCommands.cpp`). These
commands use the same pin whitelist and hard block list as the plugin GPIO host
API (`gpio_policy::isAllowed`): firmware-internal pins (TROPIC01 SPI, display
SPI, charger, USB, PSRAM) are always blocked. There is no per-command capability
check here because the user is physically holding the badge, but the commands are
AUTH-gated like the rest of the console.

### `GPIO`

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `LIST` | - | List the user-accessible pins on the whitelist |
| `MODE` | `<pin> in\|out\|out_od [pull_up\|pull_down\|none]` | Configure a pin's direction and pull |
| `WRITE` | `<pin> 0\|1` | Drive an output pin |
| `READ` | `<pin>` | Read a pin level (prints `0` or `1`) |
| `RELEASE` | `<pin>` | Reset a pin to its default state |

### `ADC`

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `READ` | `<pin>` | One-shot ADC1 reading at 12 dB attenuation; prints `raw=<value>`. ADC1 pins only: 2, 3, 4, 5, 6, 7, 9 |

### `I2C`

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `SCAN` | `<bus>` | Probe 7-bit addresses `0x08`-`0x77` on the given bus and list the ones that ACK. Bus `0` is reserved and rejected |

### `SAO`

The SAO EEPROM is a 24Cxx-style device on I2C bus 1 at address `0x50` with a
16-bit register address.

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `EEPROM_READ` | `<offset> <count>` | Read `count` bytes (1-256) and print them as hex |
| `EEPROM_WRITE` | `<offset> <hex>` | Write up to 128 bytes (hex string) to the EEPROM |
