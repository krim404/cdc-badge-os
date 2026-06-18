---
title: Installing plugins
description: Install plugins on the CDC Badge via the browser web installer, the tools/upload.py CLI, or the PLUGIN serial commands.
sidebar:
  order: 2
---

Installing a plugin means writing its files to the `plugins` partition. There are three routes: the browser web installer, the `tools/upload.py` command-line tool, and the raw `PLUGIN UPLOAD` serial commands. All three write the same files.

## Files involved

A complete install needs at least the manifest and the binary:

- `<id>.meta`: the JSON manifest (id, version, capabilities, resources).
- `<id>.wasm` (or `<id>.aot`): the WebAssembly binary. Uploading an `.aot` removes any existing `.wasm` for that id, and vice versa.
- `<id>.lang` (optional): a translation overlay for the plugin's UI strings.

The plugin id is taken from the manifest's `id` field.

## Browser web installer

The CDC Badge plugins project ships a Rust SDK, example plugins, and a Web-based installer that uploads over the browser serial port. It lives at the [cdc-badge-plugins repository](https://github.com/krim404/cdc-badge-plugins). Open its pages site, connect the badge, and follow the installer.

## CLI: tools/upload.py

`tools/upload.py` is the unified upload tool in this repository. It talks to the badge over USB-CDC serial at 115200 baud and auto-detects the port (override with `--port`). It requires `pyserial` (`pip install pyserial`).

If the firmware was built with `FEATURE_SECURE_SERIAL`, pass the badge PIN with `--pin`; the tool sends `AUTH <pin>` before any command.

Install or update a plugin (uploads meta first, then the binary, then the optional language file):

```bash
python tools/upload.py --wasm hello.wasm --meta hello.meta.json --pin 0000
python tools/upload.py --wasm hello.wasm --meta hello.meta.json --lang hello.lang.json --pin 0000
```

The id is read from `meta.json#id`. Override it with `--id`.

Other modes:

```bash
python tools/upload.py --list --pin 0000          # list installed plugins (JSON)
python tools/upload.py --info hello --pin 0000     # show one plugin's manifest details
python tools/upload.py --start hello --pin 0000    # start a plugin
python tools/upload.py --stop --pin 0000           # stop the active plugin
python tools/upload.py --delete hello --pin 0000   # delete a plugin
```

The tool can also write a UI language overlay to `/vfat/system/i18n/` and reload it (`--lang-overlay lang_<code>.json`), and stream an arbitrary file onto the partition (`--put <file> [--dir <dir>] [--name <n>]`). These use the VFAT serial shell rather than the plugin upload path.

:::caution
Each upload writes through the badge's serial console. Avoid running the serial monitor at the same time, since both contend for the same USB-CDC port.
:::

## Serial commands

Upload is exposed directly over the serial console as a binary stream. The command arms a receiver, the badge replies `READY`, then exactly `<size>` raw bytes follow, and the firmware verifies a CRC32 over the whole stream before committing the file:

```text
PLUGIN UPLOAD <id> <size> <crc32_hex>
PLUGIN UPLOAD_AOT <id> <size> <crc32_hex>
PLUGIN UPLOAD_META <id> <size> <crc32_hex>
PLUGIN UPLOAD_LANG <id> <size> <crc32_hex>
PLUGIN ABORT
```

Notes verified from the firmware:

- An upload session auto-aborts after **15 seconds** of inactivity, so a crashed client cannot wedge the console.
- Uploading a `.wasm` that overwrites the currently active foreground plugin unloads that plugin first.
- Re-uploading a `.wasm` for a plugin that is running in the background reloads the running instance so it picks up the new binary without a reboot.
- `<id>.wasm` and `<id>.aot` are mutually exclusive: uploading one removes the other.

For normal installs, prefer `tools/upload.py` or the web installer; the raw commands exist for scripting and debugging.

## After installing

The plugin appears in the on-device **Plugins** menu and in `PLUGIN LIST`. See [Managing plugins](/power/plugins/manage/) for starting, stopping, and the residency model.
