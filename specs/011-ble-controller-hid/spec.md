# Feature Specification: BLE Controller & HID

**Feature Branch**: `011-ble-controller-hid`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the single
shared BLE controller (`cdc_hal` BluetoothController) and the HID-over-BLE module (`mod_blehid`):
the peripheral single-connection model, numeric-comparison pairing and host bonding, and the
single-controller invariant that modules use `IBluetoothController` only and never touch NimBLE.

> **⚠ PROVISIONAL (WIP) NOTE**: The HID-over-BLE / BLE-vCard parts of this capability that depend on
> the BLE transport are **not yet hardware-verified**. Per the baseline Clarifications (Session
> 2026-06-14), BLE-side features (BLE HID keyboard auto-type, BLE serial console, BLE vCard exchange)
> are in-scope but Provisional; their hardware acceptance is **non-blocking** until verified on
> hardware and they MUST stay flagged WIP until then. The single-controller invariant itself
> (FR-090, the architectural rule) is normative and not WIP.

> **Source of truth**: This spec lifts requirement FR-090 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); the FR number is preserved as a cross-reference. Code is
> the ground truth for current behaviour. The owning components are the `cdc_hal` BluetoothController
> and `mod_blehid`. The badge-to-badge message-transfer framework that also rides the BLE controller
> (FR-050..054) is owned by spec `008-message-transfer`; WiFi/NTP connectivity (FR-091) is owned by
> spec `014`.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Single shared BLE controller (Priority: P1)

The device runs exactly one BLE controller as a peripheral with a single connection at a time; all
BLE-using modules go through `IBluetoothController` and never touch NimBLE directly.

**Why this priority**: A single owning controller is the architectural invariant that keeps GAP
event handling correct (Constitution I); a module touching NimBLE directly breaks GATT registration
and lifecycle teardown.

**Independent Test**: With multiple BLE-using modules enabled (HID, serial, message transfer),
confirm only one controller advertises, accepts a single connection at a time, and that no module
registers its own GAP handler.

**Acceptance Scenarios**:

1. **Given** BLE enabled with multiple BLE-using modules, **When** the device advertises and a host
   connects, **Then** the device operates as a peripheral with a single connection at a time through
   one shared controller.
2. **Given** a BLE-using module, **When** it needs BLE services, **Then** it uses the
   `IBluetoothController` API exclusively and does not add `bt` to its CMakeLists REQUIRES or touch
   NimBLE directly.
3. **Given** a single active connection, **When** a second host attempts to connect, **Then** the
   device does not accept a second simultaneous connection.

---

### User Story 2 - Pair and bond a host for HID (Priority: P1)

The holder pairs a host using numeric comparison so the badge can act as a BLE HID keyboard; the
device may remember (bond) up to 5 host devices.

**Why this priority**: BLE HID keyboard auto-type (passwords, 2FA) requires a paired, bonded host;
numeric comparison is the trust gate for that pairing.

**Independent Test (BLE-side Provisional)**: Pair a host using numeric comparison, confirm the
matching code, and confirm the badge bonds the host and can act as a BLE HID keyboard; repeat up to
5 bonded hosts.

**Acceptance Scenarios**:

1. **Given** a host initiating pairing for HID, **When** numeric comparison is shown and both sides
   confirm the matching code, **Then** the host is paired and may be bonded.
2. **Given** fewer than 5 bonded hosts, **When** a new host bonds, **Then** the device retains the
   bond (up to 5 bonded host devices).
3. **Given** a bonded host, **When** the holder triggers auto-type for a password or 2FA field,
   **Then** the field is sent to the host as BLE HID keystrokes. *(BLE-side Provisional, not
   hardware-verified.)*

---

### User Story 3 - Bonding capacity behaviour (Priority: P3)

The device defines what happens when the holder bonds beyond the supported number of host devices.

**Why this priority**: The bond table is finite; the holder needs predictable behaviour at capacity
so a new pairing does not silently fail or evict an unexpected device.

**Independent Test**: Bond 5 hosts, then bond a 6th and observe the device's bond-eviction or
rejection behaviour.

**Acceptance Scenarios**:

1. **Given** 5 bonded host devices, **When** the holder bonds a 6th, **Then** [NEEDS CLARIFICATION:
   behaviour when bonding a 6th device / bond eviction policy. (baseline B10)]

---

### Edge Cases

- **Second simultaneous connection**: the peripheral accepts only one connection at a time; a second
  connection attempt is not accepted concurrently.
- **Bond table full**: [NEEDS CLARIFICATION: behaviour when adding a 6th bonded device / bond
  eviction policy. (baseline B10)]
- **BLE HID descriptor**: [NEEDS CLARIFICATION: BLE HID descriptor/report details (report types /
  flow control) are referenced but not fully documented in-repo. (baseline B12)]
- **Module touching NimBLE**: a BLE-using module must use `IBluetoothController` only; touching
  NimBLE directly or adding `bt` to its REQUIRES violates the single-controller invariant.
- **BLE-side WIP**: BLE HID auto-type, BLE serial console and BLE vCard exchange are not
  hardware-verified.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-090**: BLE MUST operate as a peripheral with a single connection at a time; pairing for host
  HID uses numeric comparison and the device MAY bond up to 5 host devices. [NEEDS CLARIFICATION:
  behaviour when bonding a 6th device / bond eviction policy. (baseline B10)]

### Key Entities *(include if feature involves data)*

- **BLE Controller** — the single shared BLE peripheral controller (`cdc_hal` BluetoothController),
  the sole GAP event handler; accepts one connection at a time. All BLE-using modules access it via
  `IBluetoothController`; none touch NimBLE directly or add `bt` to their CMakeLists REQUIRES.
- **Host Bond** — a remembered paired host device; up to 5 may be bonded. [NEEDS CLARIFICATION:
  6th-device / eviction behaviour (baseline B10).]
- **Numeric-Comparison Pairing** — the pairing method for host HID: both sides display a code that
  the holder confirms matches.
- **BLE HID Keyboard** — the HID-over-BLE keyboard interface (`mod_blehid`) used to send password and
  2FA fields as keystrokes. [NEEDS CLARIFICATION: BLE HID descriptor/report types and flow control
  (baseline B12).] *(BLE-side Provisional, not hardware-verified.)*

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The device operates as a BLE peripheral with exactly one connection at a time; a second
  simultaneous connection is never accepted.
- **SC-002**: BLE-using modules access BLE only through `IBluetoothController` — none register their
  own GAP handler, add `bt` to REQUIRES, or touch NimBLE directly (architectural invariant, 100%).
- **SC-003**: Host HID pairing uses numeric comparison and the device bonds up to 5 host devices.
- **SC-004** *(BLE-side Provisional, HIL non-blocking)*: On hardware, a paired and bonded host
  receives a password/2FA field as BLE HID keystrokes after auto-type; verified on hardware,
  non-blocking until the BLE-side WIP parts are verified.

## Assumptions

- The single-controller invariant (one owning BluetoothController; modules use `IBluetoothController`
  only) is a Constitution I architectural rule and is normative (not WIP).
- The BLE-side HID/serial/vCard features that ride the controller are **Provisional (WIP, not
  hardware-verified)**; their hardware acceptance is non-blocking per the baseline Clarifications
  (Session 2026-06-14) until verified on hardware.
- The badge-to-badge message-transfer framework (FR-050..054) also uses this controller but is owned
  by spec `008-message-transfer`; the keyboard auto-type *content* (which fields are typed) is owned
  by the password (005) and 2FA (004) specs — this spec owns only the BLE transport and pairing.
- The bond limit (up to 5) is taken from the baseline; the 6th-device / eviction behaviour (FR-090 /
  baseline B10) and the BLE HID descriptor details (baseline B12) are open items to be resolved by
  reading the code and/or a hardware run, then documented; they are recorded here rather than guessed.
- BLE is absent from CI; protocol paths over BLE are uncoverable by host-tier tests and are verified
  only on hardware.
