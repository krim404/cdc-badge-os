---
title: Languages
description: How the CDC Badge OS language system works - the built-in English fallback, lang_<code>.json overlay files on the plugins FAT, the on-device picker, and how to add a language.
sidebar:
  order: 7
---

The badge UI is English by default and can be switched to any language for which
an overlay file is present. English is built into the firmware; every other
language is a separate file you can add or remove without rebuilding.

## How it works

Every user-facing string is looked up by a key such as `core.language`. The
lookup has two layers:

1. **Built-in English fallback.** The English text for every key is compiled
   into the firmware. Core strings live in one table; each module registers its
   own English table. English is always available and is the fallback whenever a
   translation is missing.
2. **Overlay files.** Each non-English language is a single flat JSON file named
   `lang_<code>.json` (for example `lang_de.json`), where `<code>` is the
   lower-case language code. It is a flat object of `"<key>": "<value>"` pairs.

When a non-English language is active, a string is taken from the overlay if the
overlay has that key, otherwise it falls back to the built-in English text. So a
partial overlay simply leaves untranslated strings in English.

### Where the files live

Overlay files are stored on the `plugins` FAT partition under `/vfat/system/i18n/`.
The active language's file is parsed into PSRAM at load time. German umlauts in
the file are written as real UTF-8 (`ä ö ü Ä Ö Ü ß`); the loader converts them
to the badge's internal CP437 encoding while parsing.

:::note
The same `plugins` partition holds installed plugins. Both live under the
`system/` subfolder: language files in `system/i18n/` and plugins as
`system/<id>.wasm`, so they do not interfere with each other.
:::

## The on-device picker

Open **Main menu → Settings → Language**. The picker is built dynamically:

- the first entry is always **English** (built in);
- after it comes one entry per `lang_<code>.json` file found in `/vfat/system/i18n/`.

Each overlay entry is labelled with that file's own display name, read from its
`core.lang_name` value (the endonym). For example `lang_de.json` sets
`"core.lang_name": "Deutsch"`, so it appears as "Deutsch" in the list. If a file
has no `core.lang_name`, its language code is shown instead.

Selecting a language applies it immediately, persists the choice, and reloads
the language overlay for plugins too. With no overlay files present, only
English is listed.

## Adding a language

Because the picker scans the directory, adding a language is just dropping a new
file:

1. Create `lang_<code>.json` as a flat `{ "<key>": "<value>", ... }` object.
2. Include `core.lang_name` set to the language's own display name, for example
   `"core.lang_name": "Français"`. Without it the picker shows the bare code.
3. Translate the keys you want. Any key you omit falls back to English.
4. Place the file in `/vfat/system/i18n/` on the badge.

You can write the file onto the badge over the serial console with the upload
tool (`upload.py --lang-overlay`), or seed it into the initial plugins image at
build time (`build_lang_image.py`). Both are covered in
[Companion tools](/power/companion-tools/).

:::tip[Keys must match the firmware]
Translation keys must match the keys the firmware uses. Use an existing overlay
such as `assets/i18n/lang_de.json` as the reference list of keys.
:::
