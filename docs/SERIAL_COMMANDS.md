# Serial Commands Reference

USB serial command interface for the CDC Badge.

Connect via USB CDC at **115200 baud**. Use `HELP` to list all available commands.

When `FEATURE_SECURE_SERIAL` is enabled, every command except `PING` and `AUTH`
requires an authenticated session. Use `AUTH <pin>` to log in. The session ends
on `LOGOUT`, on a wrong PIN, or after the device idle timeout.

`FEATURE_SECURE_SERIAL` defaults to 0 in
`components/cdc_core/include/cdc_core/feature_flags.h`; it is forced to 1 when
the Kconfig option `CONFIG_SECURE_SERIAL` is set.

Commands tagged `[AUTH]` additionally require authentication even when
`FEATURE_SECURE_SERIAL` is disabled. The tag is applied to anything that
mutates persistent state or accesses secret material: `REBOOT`, all NVS
mutators, every TROPIC01 slot mutator, the full TOTP / Password / GPG /
plugin module surface, and the factory-wipe commands.

## Sub-command syntax

Grouped commands are dispatched as `<GROUP> <SUBCOMMAND> [args...]`. For example:

```
NVS LIST
NVS READ display name
PIN CHANGE 1234 5678
WIFI CONNECT MyAP secret123
```

Typing the bare group name (e.g. `NVS`) or `<GROUP> HELP` prints a usage block
listing the available sub-commands. The same listing is reproduced by `HELP`
under each module section.

## Authentication

| Command | Description |
|---------|-------------|
| `AUTH <pin>` | Authenticate with PIN |
| `LOGOUT` | End authenticated session |

## System

| Command | Description |
|---------|-------------|
| `HELP` | Show available commands (includes sub-command listings) |
| `PING` | Check if device is responsive (returns PONG) |
| `STATUS` | Show system status |
| `MEM` | Show memory usage (Heap/PSRAM/NVS) |
| `MEMINFO` | Show detailed memory + FreeRTOS task info |
| `ERROR_LOG` | Show error log |
| `ERROR_LOG CLEAR` | Clear error log |
| `REBOOT` | Restart the device `[AUTH]` |
| `BOOTLOADER` | Reboot into USB download mode `[AUTH]` |

## Time

| Command | Description |
|---------|-------------|
| `GET_TIME` | Show current time |
| `GET_DATE` | Show current date |
| `SET_TIME HH:MM:SS` | Set time |
| `SET_DATE DD.MM.YYYY` | Set date |
| `SET_DATE <timestamp>` | Set date from Unix timestamp |

## Display

| Command | Description |
|---------|-------------|
| `SET_NAME <text>` | Set display name (lock screen) |
| `SET_INFO <text>` | Set info line 1 |
| `SET_INFO2 <text>` | Set info line 2 |

## NVS (Non-Volatile Storage)

Group command: `NVS <subcommand> [args]`

| Sub-command | Description |
|-------------|-------------|
| `NVS LIST [namespace]` | List NVS entries (optionally filtered by namespace) |
| `NVS READ <ns> <key>` | Read NVS key value |
| `NVS DEL <ns> [key]` | Delete a single key, or the entire namespace if key is omitted `[AUTH]` |
| `NVS CLEAR YES` | Erase entire NVS (`YES` confirmation required) `[AUTH]` |

## PIN Management

Group command: `PIN <subcommand> [args]`

| Sub-command | Description |
|-------------|-------------|
| `PIN STATUS` | Show PIN status and retry counts |
| `PIN RESET` | Reset PIN retries (debug only) |
| `PIN CHANGE <currentPin> <newPin>` | Change badge PIN (4-8 digits) `[AUTH]` |

## TROPIC01 Secure Element

Group command: `TR01 <subcommand> [args]`

| Sub-command | Description |
|-------------|-------------|
| `TR01 STATUS` | Show TR01 connection status |
| `TR01 INFO` | Show TR01 chip info (ID, firmware) |
| `TR01 SESSION` | Start/restart TR01 session |
| `TR01 SLOTS` | Show slot usage summary |
| `TR01 RMEM_READ <slot>` | Read and dump R-Memory slot |
| `TR01 ECC_DEL <slot>` | Delete ECC key slot `[AUTH]` |
| `TR01 RMEM_DEL <slot>` | Delete R-Memory slot `[AUTH]` |
| `TR01 RESYNC` | Resync TR01 session and cache |
| `TR01 CACHE_REBUILD` | Rebuild TR01 cache from chip |
| `TR01 CLEANUP` | Cleanup mismatched slots + rebuild cache `[AUTH]` |
| `TR01 WIPE CONFIRM` | Factory reset all TR01 data `[AUTH]` |

## WiFi

Group command: `WIFI <subcommand> [args]`. All WiFi commands require authentication.

| Sub-command | Description |
|-------------|-------------|
| `WIFI SCAN` | Scan for available networks |
| `WIFI STATUS` | Show WiFi state and saved configuration |
| `WIFI ON [sta\|ap\|sta_ap]` | Enable WiFi radio (default STA, auto-reconnects if saved config exists) |
| `WIFI OFF` | Disable WiFi radio |
| `WIFI CONNECT <ssid> <password>` | Connect to network and persist credentials |
| `WIFI TIMEOUT [ms]` | Get or set connect timeout (3000-60000 ms, default 15000, persisted in NVS) |
| `WIFI FORGET` | Clear all saved WiFi configuration |

## TOTP Module

Group command: `TOTP <subcommand> [args]`. All TOTP commands require authentication.

| Sub-command | Description |
|-------------|-------------|
| `TOTP LIST` | List all TOTP accounts |
| `TOTP ADD <name> <secret> [issuer] [digits] [period] [algo]` | Add TOTP account |
| `TOTP DEL <index>` | Delete TOTP account by index |
| `TOTP GET <index>` | Generate TOTP code by index |

**`TOTP ADD` Parameters:**
- `name` - Account name (required)
- `secret` - Base32 encoded secret (required)
- `issuer` - Issuer name (optional)
- `digits` - Code length: 6, 7, or 8 (default: 6)
- `period` - Time period in seconds (default: 30)
- `algo` - HMAC algorithm: `SHA1`, `SHA256`, or `SHA512` (default: SHA1)

## Password Module

Group command: `PASSWORD <subcommand> [args]`. All password commands require authentication.

| Sub-command | Description |
|-------------|-------------|
| `PASSWORD LIST` | List password entries (sorted by title) |
| `PASSWORD GET <slot>` | Show one entry (title, username, password, URL, TOTP link, notes) |
| `PASSWORD ADD <slot\|x> <title> <user\|x> <pw\|x> <url\|x> <totp\|-> [notes]` | Add entry; `x` skips a field, `x` for password generates a 16-char random one, `x` for slot picks the next free slot |
| `PASSWORD EDIT <slot> <field> <value>` | Edit one field. `field` is `title`, `username`, `password`, `url`, `totp`, or `notes`. Use `\\ ` to include spaces in `value`. |
| `PASSWORD DEL <slot>` | Delete entry by slot |

## GPG Module

Group command: `GPG <subcommand> [args]`. All GPG commands require authentication.

| Sub-command | Description |
|-------------|-------------|
| `GPG STATUS` | Show keys, fingerprints, counters |
| `GPG GENERATE <curve> <user_id>` | Generate SIG + DEC + AUT in one shot (`1` = Ed25519 for SIG/AUT, `2` = P-256 ECDSA for SIG/AUT; DEC is always P-256 ECDH) |
| `GPG EXPORT` | Print primary + subkey pubkeys as PEM |
| `GPG RESET [token]` | Two-step destructive reset (see below) |
| `GPG RECV_LIST` | List received cross-sign keys (see [CROSS_SIGNING.md](CROSS_SIGNING.md)) |
| `GPG RECV_INFO <index>` | Show details for a received key |
| `GPG RECV_DELETE <index>` | Delete a received key |
| `GPG CROSS_SIGN <index>` | Cross-sign a received key with the badge's SIG subkey |
| `GPG EXPORT_SIGNED <index>` | Print signed key as ASCII-armored OpenPGP block (importable via `gpg --import`) |

`GPG RESET` wipes all three ECC slots, the DEC backup in R-Mem 502, the AES key,
and resets PINs to factory defaults. To prevent fat-fingered loss, the command
is two-step:

```
> GPG RESET
WARNING: this wipes ALL GPG keys (SIG/DEC/AUT), the DEC backup, and PINs.
Confirm within 30s: GPG RESET A4F921

> GPG RESET A4F921
OK
```

## vCard Module (BLE Badge-to-Badge)

Group command: `VCARD <subcommand>`

| Sub-command | Description |
|-------------|-------------|
| `VCARD SET` | Set own vCard (multiline paste, terminate with `---` on its own line or `ABORT` to cancel) |
| `VCARD GET` | Show own vCard |
| `VCARD DELETE` | Delete own vCard |

## Plugin Manager

Group command: `PLUGIN <subcommand> [args]`. All plugin commands require authentication.

| Sub-command | Description |
|-------------|-------------|
| `PLUGIN LIST` | List installed plugins (JSON array of `{id, name, version}`) |
| `PLUGIN INFO <id>` | Show manifest details for one plugin |
| `PLUGIN START <id>` | Start a plugin |
| `PLUGIN STOP` | Stop the currently active plugin |
| `PLUGIN CMD <id> <args>` | Forward a command string to a plugin |
| `PLUGIN DELETE <id>` | Delete wasm + meta + lang files for plugin |
| `PLUGIN UPLOAD <id> <size> <crc32_hex>` | Upload `.wasm` payload (binary byte-stream) |
| `PLUGIN UPLOAD_AOT <id> <size> <crc32_hex>` | Upload `.aot` payload (binary byte-stream) |
| `PLUGIN UPLOAD_META <id> <size> <crc32_hex>` | Upload `.meta` payload (binary byte-stream) |
| `PLUGIN UPLOAD_LANG <id> <size> <crc32_hex>` | Upload `.lang` payload (binary byte-stream) |
| `PLUGIN ABORT` | Abort an active upload session |
| `PLUGIN DEBUG` | Toggle verbose `PLG_*` / `host_*` logging |

After issuing an `UPLOAD*` sub-command the device responds with `READY`, then
accepts exactly `<size>` raw bytes from the serial stream. CRC-32 is computed
on the fly and verified against `<crc32_hex>` (IEEE 802.3) before the partial
file is renamed in place. A `READY` session aborts after 15 s of inactivity.

## vFAT Shell

Group command: `VFAT <subcommand> [args]`. Requires authentication. Operates
on the plugins FAT partition with a stateful working directory (reset to root
when the serial session locks).

| Sub-command | Description |
|-------------|-------------|
| `VFAT LIST` | List the current directory |
| `VFAT CD <path>` | Change directory (`..` = up, `/` = root) |
| `VFAT PWD` | Print the working directory |
| `VFAT GET <file>` | Print a file's contents |
| `VFAT PUT <file> <text>` | Write text (`\n` -> newline) |
| `VFAT RECEIVE <file> <size> <crc32_hex>` | Stream a binary file into the current dir (same protocol as `PLUGIN UPLOAD`) |
| `VFAT DELETE <file>` | Delete a file |
| `VFAT MKDIR <name>` | Create a directory |
| `VFAT RMDIR <name>` | Remove an empty directory |
| `VFAT FREE` | Show partition usage (total / used / free) |

`VFAT RECEIVE` is the single generic file-receive path: it replies `READY`,
then accepts exactly `<size>` raw bytes verified against `<crc32_hex>`.

## i18n Overlay

Group command: `LANG <subcommand> [args]`. Requires authentication.

UI languages are per-language files `/plugins/i18n/lang_<code>.json` (flat
key/value JSON; the `core.lang_name` value is the language's own display name).
Upload one with `VFAT RECEIVE i18n/lang_<code>.json <size> <crc>` then
`LANG RELOAD`; it appears in the on-device language picker automatically.

| Sub-command | Description |
|-------------|-------------|
| `LANG INFO` | Show active language and available languages under `/plugins/i18n/` |
| `LANG RELOAD` | Rescan + reload language overlays from `/plugins/i18n/` |

## Examples

```bash
# Set time from Unix timestamp
echo "SET_DATE $(date +%s)" > /dev/ttyACM0

# Add TOTP account
echo "TOTP ADD GitHub JBSWY3DPEHPK3PXP" > /dev/ttyACM0

# Add TOTP with all options
echo "TOTP ADD AWS HXDMVJECJJWSRB3HWIZR4IFUGFTMXBOZ Amazon 6 30" > /dev/ttyACM0

# Show memory usage
echo "MEM" > /dev/ttyACM0

# WiFi: enable STA mode and connect
echo "AUTH 1234" > /dev/ttyACM0
echo "WIFI ON sta" > /dev/ttyACM0
echo "WIFI CONNECT MyAP secretPassword" > /dev/ttyACM0

# NVS: inspect and delete a key
echo "NVS LIST display" > /dev/ttyACM0
echo "NVS DEL display name" > /dev/ttyACM0

# Factory reset (dangerous!)
echo "TR01 WIPE CONFIRM" > /dev/ttyACM0
```
