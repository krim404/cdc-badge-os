---
title: File storage & NVS tools
description: The on-device vFAT file explorer and the NVS browser/editor - what they let you do, their default-enabled status, and the serial equivalents.
sidebar:
  order: 8
---

Two modules expose the badge's internal storage to a power user: a file explorer
for the `plugins` FAT partition, and a browser for the key-value NVS store. Both
modules are enabled by default.

## vFAT file explorer (`mod_vfat`)

The vFAT module browses the FAT partition that holds plugins and language files.
It is enabled by default.

### On-device

When `mod_vfat` is enabled, an explorer entry appears in the Expert menu
(**Main menu → Tools → Expert**). It opens at the partition root so you can
navigate directories and view the files stored there.

### Serial shell

The module also registers a `VFAT` serial command. It is AUTH-gated: when secure
serial is enabled you authenticate first. The shell has a working directory and
these sub-commands:

| Sub-command | Action |
| --- | --- |
| `LIST` | List the current directory. |
| `PWD` | Print the working directory. |
| `CD <path>` | Change directory (`..` goes up). |
| `GET <file>` | Print a file's contents. |
| `PUT <file> <text>` | Write text to a file (`\n`, `\t`, `\\` are unescaped). |
| `RECEIVE <file> <size> <crc32_hex>` | Stream a binary file in, verified by size and CRC32. |
| `DELETE <file>` | Delete a file. |
| `MKDIR <name>` | Create a directory. |
| `RMDIR <name>` | Remove an empty directory. |
| `FREE` | Show partition usage (total / used / free in KB). |

`RECEIVE` is the binary upload path the host-side tools use to stream files
(plugins, language overlays, backup containers) onto the badge. The companion
[`upload.py`](/power/companion-tools/) drives it for arbitrary files via `--put`.

## NVS browser / editor (`mod_nvsedit`)

The NVS module browses the badge's non-volatile key-value store, where modules
keep their settings. It is enabled by default.

### Read-only by default

Whether the module can delete entries depends on a build-time feature flag
(`FEATURE_NVS_EDIT`), which is **off** by default. With the flag off the module
is a read-only **NVS Browser**; deletes are blocked. Only a firmware built with
`FEATURE_NVS_EDIT=1` exposes the writable **NVS Editor** that can delete keys and
namespaces.

### On-device

The entry sits in the **Tools** menu (labelled "NVS Browser" when read-only, or
"NVS Editor" when deletes are enabled). Opening it asks for confirmation, then
lets you:

- list the NVS namespaces;
- list the keys in a namespace, each shown with its type (`u8`, `i32`, `str`,
  `blob`, and so on);
- open a key to see a formatted preview of its value (integers in decimal and
  hex, strings inline, short blobs as hex, larger blobs as a byte count).

When the writable editor is built in, a per-item context menu offers **Delete
Key** and **Delete NS** (delete an entire namespace). These deletes are
irreversible; the editor warns about this before you enter it.

:::caution
NVS holds the settings other modules depend on. Deleting keys or namespaces can
reset features or leave a module without its stored state. Only use the writable
editor if you know which key you are removing.
:::

### No serial equivalent

The NVS module does not register a serial command. NVS browsing and editing are
on-device only. To wipe all settings from the host instead, re-flash with the
NVS partition erased (see the companion
[`flash_firmware.py --erase-nvs`](/power/companion-tools/)).
