# Feature Specification: Serial Console & Host Tooling

**Feature Branch**: `012-serial-console`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the
line-oriented serial console over USB CDC and BLE, its AUTH gate, and the raw upload mode used to
stream plugin/file binaries onto the device.

> **Source of truth**: This spec lifts requirements FR-080..082 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning component is `serial_cmd`. The badge-PIN
> lockout state shared by the AUTH gate is owned by spec `002-lock-pin-duress`; this spec only
> specifies the console-side gating contract.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Manage and diagnose the badge from a host (Priority: P1)

A host computer connects over USB CDC (115200 baud) or over BLE Nordic UART and drives the badge
with line-oriented text commands grouped into families for system, time, display, storage (NVS),
PIN, secure element (TR01), WiFi, modules, GPG, 2FA, password, vCard, backup, plugin/file
management (PLUGIN/VFAT), i18n and GPIO/ADC/I2C/SAO.

**Why this priority**: The serial console is the primary host-side management, provisioning and
diagnostics interface; companion tools (`flash_firmware.py`, `upload.py`, `ble_serial.py`,
`backup.py`, `coredump.py`, the web flasher and the plugin web installer) all speak through it.

**Independent Test**: Connect a host to the USB CDC port at 115200 baud, send `HELP`, and confirm
the device lists the available command families.

**Acceptance Scenarios**:

1. **Given** a host connected over USB CDC at 115200 baud, **When** it sends a recognised command
   line terminated by a newline, **Then** the device parses it, executes the corresponding handler,
   and returns a line-oriented response.
2. **Given** the same console, **When** the host connects instead over BLE (Nordic UART), **Then**
   the identical command set is available over that transport.
3. **Given** an unrecognised command, **When** it is sent, **Then** the device returns a clear error
   without executing anything.

---

### User Story 2 - Gate secret-bearing commands behind the badge PIN (Priority: P1)

When the secure-serial gate is active, every command except `PING` and `AUTH` is refused until the
host has authenticated with `AUTH <pin>` using the badge PIN. The console shares the lock screen's
brute-force lockout state, so guessing over serial counts against the same retry budget.

**Why this priority**: Secret-bearing commands (TOTP/PASSWORD/GPG/BACKUP/TR01/NVS) must not be
reachable by anyone who can plug in a cable; the AUTH gate is the access-control boundary for the
console.

**Independent Test**: With the secure-serial gate active, send a secret-bearing command before
`AUTH` and confirm it is refused; then send `AUTH <correct-pin>` and confirm the same command now
succeeds.

**Acceptance Scenarios**:

1. **Given** an active secure-serial gate and no prior `AUTH`, **When** the host sends any command
   other than `PING` or `AUTH`, **Then** the device refuses it as unauthenticated.
2. **Given** an active secure-serial gate, **When** the host sends `AUTH` with the correct badge
   PIN, **Then** the session becomes authenticated and the full command set is available.
3. **Given** an active secure-serial gate, **When** the host sends `AUTH` with a wrong PIN, **Then**
   the attempt counts against the shared lock-screen retry budget and the same 60-second lockout
   applies (see spec 002).
4. **Given** an authenticated session, **When** the configured idle timeout elapses without a
   command, **Then** the session is deauthenticated and a fresh `AUTH` is required.

---

### User Story 3 - Stream a plugin or file onto the device (Priority: P2)

A host uploads a plugin binary or other file by switching the console into a raw byte-streaming mode
with a declared size, then sending the bytes; the device verifies a CRC-32 over the received data.

**Why this priority**: Installing plugins and pushing files without reflashing firmware is a core
extensibility flow; it must be robust against a crashed or stalled client wedging the console.

**Independent Test**: Initiate an upload with a declared size, stream the bytes, and confirm the
device accepts the file only when the CRC-32 matches; then start an upload and stop sending,
confirming the session auto-aborts after the inactivity window.

**Acceptance Scenarios**:

1. **Given** an upload command with a declared byte size, **When** the host streams exactly that
   many bytes and the CRC-32 matches, **Then** the device stores the file and reports success.
2. **Given** a transfer whose received CRC-32 does not match the expected value, **When** the stream
   completes, **Then** the device rejects the upload.
3. **Given** an upload session that stalls, **When** 15 seconds pass with no inactivity, **Then** the
   session auto-aborts and the console returns to command mode.

---

### Edge Cases

- **Upload stall**: an upload session auto-aborts after 15 seconds of inactivity so a crashed client
  cannot wedge the console.
- **Unauthenticated secret command**: with the gate active, a secret-bearing command sent before
  `AUTH` is refused, not silently queued.
- **AUTH under lockout**: serial `AUTH` attempts share the lock-screen lockout, so the 60-second
  lockout blocks both transports at once (state owned by spec 002).
- **Gate disabled**: when the secure-serial gate is not active, commands are accepted without an
  `AUTH` (the shipped default depends on the build configuration — see FR-081 clarification).

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-080**: The device MUST expose a line-oriented serial console over USB CDC (115200 baud) and
  over BLE (Nordic UART), with command families for system, time, display, storage (NVS), PIN,
  secure element (TR01), WiFi, modules, GPG, 2FA, password, vCard, backup, plugin/file management
  (PLUGIN/VFAT), i18n and GPIO/ADC/I2C/SAO.
- **FR-081**: When the secure-serial gate is active, all commands except `PING` and `AUTH` MUST
  require a prior `AUTH <pin>` using the badge PIN, sharing lockout state with the lock screen, with
  a session idle timeout.
  [NEEDS CLARIFICATION: the real default of the secure-serial gate is **0/off**: `feature_flags.h`
  defaults `FEATURE_SECURE_SERIAL` to 0 unless Kconfig `CONFIG_SECURE_SERIAL` is set, even though
  `serial-console.md` states it is "enabled by default"; the Kconfig default for
  `CONFIG_SECURE_SERIAL` determines the shipped default. (baseline D1)]
  [NEEDS CLARIFICATION: exact AUTH session idle timeout value (read from source). (baseline B9)]
- **FR-082**: Plugin/file uploads MUST switch to a raw byte-streaming mode with a declared size and
  CRC-32 verification, and auto-abort after 15 seconds of inactivity.

### Key Entities *(include if feature involves data)*

- **Serial session** — a connection over one transport (USB CDC or BLE Nordic UART) with an
  authentication state (unauthenticated vs. AUTH-passed) and an idle timer; the AUTH state shares
  the lock-screen retry budget and 60-second lockout (owned by spec 002).
- **Command** — a single newline-terminated text line dispatched to a registered handler within one
  of the command families; each command is either ungated (`PING`, `AUTH`) or gated behind AUTH when
  the secure-serial gate is active.
- **Upload session** — a raw byte-streaming transfer with a declared size and an expected CRC-32,
  bounded by a 15-second inactivity auto-abort, used for plugin/file (PLUGIN/VFAT) uploads.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A host sending a valid command over USB CDC at 115200 baud (or over BLE Nordic UART)
  receives the corresponding line-oriented response, with the identical command set available on
  both transports.
- **SC-002**: With the secure-serial gate active, every command except `PING`/`AUTH` is refused
  until a correct `AUTH <pin>` succeeds, and a wrong serial `AUTH` consumes the same shared retry
  budget as the lock screen 100% of the time.
- **SC-003**: An authenticated session is deauthenticated after the idle timeout, requiring a fresh
  `AUTH` before any further gated command.
- **SC-004**: A raw upload is accepted only when the received CRC-32 matches the declared data, and
  a stalled upload auto-aborts after 15 seconds of inactivity 100% of the time, returning the
  console to command mode.

## Assumptions

- The badge-PIN brute-force lockout state shared by the AUTH gate is owned and verified by spec
  `002-lock-pin-duress`; this spec specifies only the console-side gating contract.
- The shipped default of the secure-serial gate is determined by the build configuration; per
  baseline D1 the code default is off (`FEATURE_SECURE_SERIAL=0`) unless Kconfig
  `CONFIG_SECURE_SERIAL` enables it, and a release build (firmware version ≥ 1.0) is required to set
  `FEATURE_SECURE_SERIAL=1` (baseline SC-013).
- Both the USB CDC and BLE Nordic UART transports expose the same command set; BLE serial console is
  flagged work-in-progress and its hardware acceptance is non-blocking until verified.
- Companion host tools (`flash_firmware.py`, `upload.py`, `ble_serial.py`, `backup.py`,
  `coredump.py`, the web flasher and the plugin web installer) consume this console and are out of
  scope of this spec beyond the contract they depend on.
