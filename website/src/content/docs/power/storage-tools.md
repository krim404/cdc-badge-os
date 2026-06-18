---
title: File storage & NVS tools
description: The on-device vFAT file explorer (main-menu Files and Expert System Files), the NVS browser/editor, and the default-off USB mass-storage drive - what they let you do, their default status, and the serial equivalents.
sidebar:
  order: 8
---

Three modules expose the badge's internal storage: a file explorer for the vFAT
partition, reachable both as a user-facing **Files** entry in the main menu and
as **System Files** in the Expert menu; a browser for the key-value NVS store;
and a USB mass-storage service that presents the vFAT volume to a connected
computer as a removable drive. The file explorer and NVS browser are enabled by
default; the USB mass-storage service is disabled by default.

## vFAT file explorer (`mod_vfat`)

The vFAT module browses the FAT partition that holds your files alongside the
installed plugins and language overlays. It is enabled by default.

System files (installed plugins and language overlays) live in a `system/`
subfolder at the partition root, keeping them apart from your own files.

### On-device

When `mod_vfat` is enabled the explorer is reachable two ways:

- **Files** in the **main menu** opens the user file area. It browses the
  partition root and **hides** the `system/` folder, so you can manage your own
  files without reaching the plugin and language files. A fresh badge ships with
  `demo.jpg` and `demo.md` here.
- **System Files** under the **Expert** menu (**Main menu → Tools → Expert**)
  shows everything, including `system/`. Use it when you need to inspect the
  installed plugin or language files.

Both open at their starting directory so you can navigate directories and view
the files stored there.

### Viewing files

Selecting a text-like file (`.txt`, `.json`, `.csv`, `.log`, `.cfg`, `.ini`,
`.yaml`, `.xml`, `.html`, `.md`, ...) opens it in the scrollable text viewer.
Scroll with **2** / **8** and leave with **N**.

`.md` and `.markdown` files open rendered, not as raw text. The renderer
supports headings, ordered and unordered lists, inline and fenced code blocks,
block quotes, horizontal rules, **bold** and ~~strikethrough~~ text, and links
shown as underlined text. Italics are shown as plain text (the body font has no
slanted cut).
Headings use the larger bold fonts; the available sizes are assigned to the
heading levels actually present in the document, smallest first, so a document
that uses a single level does not jump to the largest font. Documents are
rendered up to 64 KB; longer files are truncated with an indicator. Unsupported
constructs (for example tables) fall back to plain text.

Markdown task lists are interactive: a line such as `- [ ] item` shows a
checkbox. Select one with **4 / 6** and toggle it with **Y**; the change is
written back to the file. Links and images are selectable the same way: a
`[text](url)` link opens in the browser, and an `![alt](file)` image opens the
referenced local file in the image viewer.

`.png`, `.jpg` and `.jpeg` files open in the image viewer: the picture is
decoded, dithered to black and white and scaled to fit the screen. Press **5**
to switch between fit-to-screen and actual size; in actual size **2/4/6/8** pan
across the image. **N** returns to the list. Sources up to 512 KB and about one
megapixel are accepted; larger or unreadable files show a message instead.
Baseline and progressive JPEGs are both supported.

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

## USB mass storage (`mod_msc`)

The USB mass-storage module exposes the badge's vFAT volume to a connected
computer as a removable drive, so you can copy files on and off with a normal
file manager. It is **disabled by default**, like the other optional USB
services, and is enabled per device.

### Enabling

Toggle `mod_msc` in **Main menu → Tools → Expert → Modules**. When enabled, the
badge presents the drive to the host; disabling it (or unplugging) removes the
drive. The chosen state persists across reboots. A status entry under
**Settings** shows whether the service is active and whether a host is connected.

### Behaviour

- The drive shows your files at the volume root. The `system/` folder (installed
  plugins and language overlays) is marked hidden and read-only so a host file
  manager hides it and refuses to change it.
- Files copied onto the drive appear in the on-device **Files** browser after the
  host ejects or the cable is unplugged.
- While a host has the drive mounted, the badge does not write to the volume: the
  `VFAT PUT/RECEIVE/DELETE/MKDIR/RMDIR` serial commands and plugin uploads report
  that storage is busy until the host disconnects.
- The USB serial console keeps working while the drive is mounted.

The service shares the USB endpoint budget with the other USB interfaces (FIDO2,
USB keyboard, OTP, CCID/GPG). If too many of those are active, enabling
`mod_msc` reports a failure in the Modules list instead of starting; disable
another USB service and try again.

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
