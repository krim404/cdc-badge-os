# Feature Specification: Internationalisation

**Feature Branch**: `015-i18n`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the
internationalisation system: the always-available in-firmware English fallback, loadable flat-JSON
language overlays, endonym labelling, UTF-8 → CP437 conversion at parse time, and per-key fallback
to English.

> **Source of truth**: This spec lifts requirement FR-100 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); the FR number is preserved as a cross-reference. Code
> is the ground truth for current behaviour. The owning component is `cdc_ui` `I18n`. The CP437
> display rendering pipeline that consumes the converted strings is owned by the UI specs
> (`014`/`016`) and only referenced here.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Always have a usable language (Priority: P1)

English is the always-available, in-firmware fallback language. Every user-facing string has an
English value compiled into the firmware, so the device is fully usable in English even with no
language overlay present on the device.

**Why this priority**: A device that can drop into an unreadable state because a translation is
missing is unusable; the in-firmware English fallback guarantees the UI is always legible.

**Independent Test**: With no language file present, confirm the entire UI renders in English; for
any key, confirm an English value is shown.

**Acceptance Scenarios**:

1. **Given** a device with no language overlay on the plugins partition, **When** the holder uses
   the UI, **Then** every user-facing string renders in English from the in-firmware fallback.
2. **Given** any UI string key, **When** it is resolved, **Then** an English value is always
   available as a fallback.

---

### User Story 2 - Load an additional language as an overlay (Priority: P1)

Additional languages are loaded as flat JSON overlay files (`/plugins/i18n/lang_<code>.json`) parsed
into PSRAM at boot. Each file is a flat `{ "<key>": "<value>" }` object; UTF-8 is converted to
CP437 at parse time so the display pipeline renders it correctly. Adding a language is just dropping
a new `lang_<code>.json` file.

**Why this priority**: Loadable overlays let new languages be added without reflashing firmware,
which is the whole point of the overlay model.

**Independent Test**: Drop a `lang_<code>.json` file with a few translated keys onto the plugins
partition, reboot, switch to that language, and confirm the translated keys render correctly
(including any umlauts as CP437 glyphs).

**Acceptance Scenarios**:

1. **Given** a `lang_<code>.json` flat-JSON file under `/plugins/i18n/`, **When** the device boots,
   **Then** the file is parsed into PSRAM as a language overlay.
2. **Given** a value containing UTF-8 multibyte characters (e.g. umlauts), **When** the file is
   parsed, **Then** the value is converted to CP437 so it renders correctly on the display.
3. **Given** a new `lang_<code>.json` dropped onto the partition, **When** the device boots, **Then**
   the language becomes available with no firmware change.

---

### User Story 3 - Pick a language by its own name and fall back per key (Priority: P2)

The language picker lists English (always) plus every overlay found, each labelled by that file's
`core.lang_name` endonym (its own display name). When a selected overlay is missing a particular
key, that single key falls back to its English value rather than showing a blank or a raw key.

**Why this priority**: Endonym labelling lets a speaker recognise their language, and per-key
fallback keeps a partially translated overlay fully usable instead of breaking on the first missing
key.

**Independent Test**: Provide an overlay with `core.lang_name` set and a few keys translated but
others omitted; confirm the picker shows the endonym label, the translated keys render in that
language, and the omitted keys render in English.

**Acceptance Scenarios**:

1. **Given** one or more language overlays, **When** the holder opens the language picker, **Then**
   it lists English plus every overlay found, each labelled by its `core.lang_name` endonym.
2. **Given** a selected overlay missing a key, **When** that key is resolved, **Then** it falls back
   to the English in-firmware value (per-key fallback), not a blank or raw key.

---

### Edge Cases

- **Missing key in overlay**: a key absent from the selected overlay falls back to its English
  in-firmware value (per-key fallback), never a blank or raw key.
- **No overlay present**: with no `lang_<code>.json` on the partition, the picker still lists English
  and the UI is fully usable.
- **UTF-8 in overlay values**: multibyte UTF-8 (e.g. umlauts) is converted to CP437 at parse time;
  in-code English fallbacks are ASCII and bypass the loader.
- **Endonym label**: an overlay's `core.lang_name` provides its own display name in the picker; a
  missing endonym leaves the language unlabelled by its own name.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-100**: English MUST be the always-available in-firmware fallback language; additional
  languages MUST be loadable as flat JSON overlays (`/plugins/i18n/lang_<code>.json`) parsed into
  PSRAM, labelled by each file's `core.lang_name` endonym, with UTF-8 → CP437 conversion at parse
  time and per-key fallback to English.

### Key Entities *(include if feature involves data)*

- **In-firmware English fallback** — the always-available, ASCII, compiled-in string table covering
  every user-facing key (`core.*` plus per-module tables registered at runtime); the source of
  per-key fallback.
- **Language overlay** — a flat JSON file `/plugins/i18n/lang_<code>.json` of `{ "<key>": "<value>" }`
  pairs on the plugins partition, parsed into PSRAM at boot; carries `core.lang_name` as its endonym
  label. Adding a language is dropping a new such file.
- **Translation key** — a string key (e.g. `core.something`, `mod_*.something`) resolved against the
  selected overlay first, then the English fallback per key.
- **Endonym (`core.lang_name`)** — each overlay's own display name, used to label it in the language
  picker.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With no language overlay present, 100% of user-facing strings render in English from
  the in-firmware fallback.
- **SC-002**: A `lang_<code>.json` overlay dropped onto the plugins partition becomes selectable
  after boot with no firmware change, and its UTF-8 values (including umlauts) render correctly as
  CP437 on the display.
- **SC-003**: The language picker lists English plus every overlay found, each labelled by its
  `core.lang_name` endonym.
- **SC-004**: A key missing from the selected overlay falls back to its English value 100% of the
  time (no blank or raw-key display).

## Assumptions

- The CP437 display rendering pipeline that consumes the converted strings is owned by the UI specs
  (`014`/`016`); this spec owns only the resolution, overlay loading and UTF-8 → CP437 conversion.
- Language overlays live on the plugins FAT partition under `/plugins/i18n/`; the partition and
  upload/file management are owned by the serial-console (`012`) and plugin (`010`) specs.
- In-code English fallbacks are ASCII and bypass the loader; only overlay values pass through the
  UTF-8 → CP437 conversion.
- Currently shipped languages are English (in-code fallback, always available) and German
  (`lang_de.json`); the overlay model supports adding more by dropping files.
