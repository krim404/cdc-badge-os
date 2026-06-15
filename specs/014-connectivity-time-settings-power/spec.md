# Feature Specification: Connectivity, Time, Settings & Power

**Feature Branch**: `014-connectivity-time-settings-power`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns WiFi setup and
NTP time sync, the fixed Settings menu, light/deep sleep and battery/charge status, and the
e-paper refresh-mode discipline.

> **Source of truth**: This spec lifts requirements FR-091..094 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are WiFi, the settings subsystem,
> the sleep manager, and `cdc_os_ui`. BLE peripheral/bonding (FR-090) is owned by spec
> `011-ble-controller-hid`; the auto-lock inactivity behaviour (FR-005) is owned by spec
> `002-lock-pin-duress`; both are only referenced here.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect to WiFi and sync the clock (Priority: P1)

The holder sets up WiFi (by scanning for a network or entering an SSID manually, choosing the
encryption, and using DHCP or a static IP), the device remembers the WiFi intent across reboot, and
it syncs its clock over NTP.

**Why this priority**: A correct clock is required for TOTP and for any time-dependent display, and
WiFi is the only on-device path to obtain network time without a host.

**Independent Test**: Run WiFi setup, connect to a known network, trigger NTP sync, and confirm the
device clock matches network time; reboot and confirm the WiFi intent is remembered.

**Acceptance Scenarios**:

1. **Given** the WiFi setup flow, **When** the holder scans for networks or enters an SSID manually,
   selects the encryption, and chooses DHCP or a static IP, **Then** the device connects and stores
   the configuration.
2. **Given** a connected device, **When** NTP sync runs against the configured time servers
   (pool.ntp.org, time.google.com), **Then** the device clock is set to network time.
3. **Given** a device that has connected to WiFi, **When** it reboots, **Then** the WiFi intent is
   remembered (the device re-attempts the remembered connection).

---

### User Story 2 - Adjust device settings from a fixed menu (Priority: P2)

The Settings menu exposes a fixed, ordered set of options the holder can adjust: brightness,
language, timezone, sleep interval, badge text (3 lines), date, time, and change PIN.

**Why this priority**: The Settings menu is the single on-device place to configure display, locale,
power and identity; a fixed ordered set keeps the UI predictable.

**Independent Test**: Open Settings and confirm the options appear in the fixed order; change
brightness and timezone and confirm the changes take effect.

**Acceptance Scenarios**:

1. **Given** the Settings menu, **When** the holder opens it, **Then** it shows exactly the fixed
   ordered set: brightness, language, timezone, sleep interval, badge text (3 lines), date, time,
   change PIN.
2. **Given** a settings option, **When** the holder changes it, **Then** the new value takes effect
   and persists across reboot.

---

### User Story 3 - Conserve power with sleep and show battery status (Priority: P2)

The device enters light sleep when idle on the lock screen and wakes quickly; holding the back key
enters deep sleep. The lock screen shows battery level, charging state and status icons.

**Why this priority**: Battery life is essential for a pocket-carried badge, and visible battery /
charge state lets the holder trust the device before relying on it.

**Independent Test**: Leave the device idle on the lock screen and confirm it enters light sleep and
wakes quickly; hold the back key and confirm it enters deep sleep; confirm the lock screen shows
battery level and charging state.

**Acceptance Scenarios**:

1. **Given** the device idle on the lock screen, **When** the idle interval elapses (~2 min),
   **Then** it enters light sleep and wakes quickly on input.
2. **Given** any screen, **When** the holder holds the back key (~5 s), **Then** the device enters
   deep sleep.
3. **Given** the lock screen, **When** the device is on battery or charging, **Then** it displays
   the battery level, charging state and status icons.

---

### User Story 4 - Render without unnecessary full refreshes (Priority: P3)

The e-paper display renders CP437 text and supports FULL, PARTIAL and PARTIAL_LIGHT refresh modes.
The lock-screen clock uses PARTIAL_LIGHT and is never promoted to a full refresh.

**Why this priority**: E-paper full refreshes are slow and flicker; keeping the clock on
PARTIAL_LIGHT avoids periodic disruptive full refreshes during normal lock-screen display.

**Independent Test**: Leave the device on the lock screen and observe the clock updating via light
partial refreshes without periodic full-screen refreshes.

**Acceptance Scenarios**:

1. **Given** the lock screen displaying the clock, **When** the clock updates, **Then** it uses a
   PARTIAL_LIGHT refresh and is never promoted to a FULL refresh.
2. **Given** any displayed text, **When** it is rendered, **Then** it is rendered as CP437.

---

### Edge Cases

- **E-paper staleness**: a momentary "stale" look between refreshes is expected; the lock-screen
  clock uses a light partial refresh that is never promoted to a full refresh.
- **WiFi connect failure / no NTP**: WiFi scan/connect and NTP sync are bounded by timeouts; on
  failure the device keeps its current clock and does not block (exact timeout values per
  baseline B8).
- **WiFi intent across reboot**: the device remembers the intent to connect and re-attempts after a
  reboot rather than requiring re-entry of credentials.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-091**: The device MUST support WiFi setup (scan or manual SSID, encryption selection, DHCP or
  static IP) and NTP time sync (pool.ntp.org, time.google.com), remembering WiFi intent across
  reboot.
  [NEEDS CLARIFICATION: exact WiFi scan/connect/NTP timeout values — docs say "about 10s" / "15s" /
  "10s". (baseline B8)]
- **FR-092**: The Settings menu MUST expose a fixed ordered set: brightness, language, timezone,
  sleep interval, badge text (3 lines), date, time, change PIN.
- **FR-093**: The device MUST support light sleep (idle ~2 min on lock screen, fast wake) and deep
  sleep (hold the back key ~5 s), and display battery level, charging state and status icons on the
  lock screen.
- **FR-094**: The display pipeline MUST render CP437 text and support FULL / PARTIAL / PARTIAL_LIGHT
  refresh modes, where the lock-screen clock uses PARTIAL_LIGHT and is never promoted to a full
  refresh.

### Key Entities *(include if feature involves data)*

- **WiFi Configuration** — SSID, password, encryption, IP mode (DHCP or static), and an intent flag
  remembered across reboot; stored in NVS (encrypted only inside a backup).
- **Time / NTP state** — the device clock and timezone, synced over NTP against pool.ntp.org and
  time.google.com when connected.
- **OS Settings** — the fixed ordered set surfaced by the Settings menu: brightness, language,
  timezone, sleep interval, badge text (3 lines), date, time, change PIN; persisted in NVS.
- **Power / sleep state** — light-sleep entry on lock-screen idle (~2 min) with fast wake, deep-sleep
  entry on a ~5 s back-key hold, plus battery level and charging state shown via lock-screen status
  icons.
- **Display refresh mode** — FULL / PARTIAL / PARTIAL_LIGHT; the lock-screen clock uses
  PARTIAL_LIGHT and is never promoted to FULL. Text is rendered as CP437.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The holder can complete WiFi setup (scan or manual SSID, encryption, DHCP or static
  IP), connect, and obtain network time via NTP; after a reboot the WiFi intent is remembered and
  re-attempted.
- **SC-002**: The Settings menu always presents the fixed ordered set, and a changed setting takes
  effect and persists across a reboot 100% of the time.
- **SC-003**: The device enters light sleep after the lock-screen idle interval and wakes quickly,
  enters deep sleep on the back-key hold, and the lock screen reflects battery level and charging
  state.
- **SC-004**: The lock-screen clock updates only via PARTIAL_LIGHT refreshes and is never promoted
  to a FULL refresh during normal lock-screen display.

## Assumptions

- BLE peripheral operation and host bonding (FR-090) are owned by spec `011-ble-controller-hid`; the
  auto-lock inactivity return-to-lock-screen (FR-005) is owned by spec `002-lock-pin-duress`. This
  spec references both but owns neither.
- Hardware is CDC Badge v1.0/v1.1 (ESP32-S3, GDEY029T94 e-paper, BQ25895 charger); hardware-backed
  acceptance (sleep, battery telemetry, refresh discipline) is verified on device and is non-blocking
  on the host test tier.
- The exact WiFi scan/connect and NTP timeout values (baseline B8) are read from source; the
  approximate values ("about 10s" / "15s" / "10s") are placeholders until confirmed.
- The display text pipeline is canonical CP437; user/i18n text is drawn via the CP437-safe render
  path (owned by the UI specs), not the raw print overload.
