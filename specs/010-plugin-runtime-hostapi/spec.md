# Feature Specification: Plugin Runtime & Host API

**Feature Branch**: `010-plugin-runtime-hostapi`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the sandboxed
WebAssembly plugin runtime: the WAMR runtime and its PSRAM linear memory, the manifest capability
model and host-side pointer validation, the GPIO/PWM/ADC/I2C hardware policy, host-API versioning
and the canonical/SDK byte-mirror, and the `background`/`autoload`/`prevent_sleep` lifecycle flags.

> **Source of truth**: This spec lifts requirements FR-071..075 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are `plugin_manager` and
> `wamr_runtime`. The self-destruct/factory-wipe mechanics (FR-070) are owned by spec
> `013-persistence-factory-reset` and are out of scope here. The plugin-facing `host_msg_*` transfer
> API is exposed by the framework owned by spec `008-message-transfer`.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Install and run a sandboxed plugin (Priority: P1)

The holder installs a WebAssembly plugin without reflashing firmware; the plugin runs inside the
WAMR sandbox with its linear memory in PSRAM and presents its UI.

**Why this priority**: Running third-party plugins without a reflash is the entire purpose of the
plugin runtime; everything else gates or extends this path.

**Independent Test**: Upload a plugin and its manifest to the plugins partition with a compatible
host-API level, start it, and confirm it runs and presents its UI.

**Acceptance Scenarios**:

1. **Given** a plugin binary and manifest, **When** it is uploaded to the plugins partition and its
   required host-API level is compatible, **Then** it can be started and presents its UI.
2. **Given** a started plugin, **When** it runs, **Then** its WebAssembly linear memory is allocated
   in PSRAM (16–4096 KB, default 64 KB) and it executes in the WAMR interpreter (native AOT disabled
   by default).
3. **Given** a plugin whose required host-API major does not match the firmware, or whose required
   minor exceeds the firmware minor, **When** it is loaded, **Then** it is rejected as incompatible.

---

### User Story 2 - Enforce the capability sandbox (Priority: P1)

Every host function checks the plugin's manifest capabilities and re-validates plugin-supplied
pointers/lengths against linear memory, so a plugin cannot access undeclared resources or read/write
out of bounds.

**Why this priority**: The sandbox is the trust boundary of the device (Constitution III,
NON-NEGOTIABLE); a single escape defeats the security model.

**Independent Test**: From a plugin, call a host function with an undeclared capability and with an
out-of-bounds pointer/length and confirm both are rejected without crashing the device.

**Acceptance Scenarios**:

1. **Given** a plugin calling a host function for a capability not in its manifest, **When** the call
   is made, **Then** the host rejects it.
2. **Given** a plugin passing a pointer/length that exceeds its linear memory, **When** the host
   function runs, **Then** the host validates the full extent (not just one byte) and rejects the
   call; a bare pointer is only 1-byte-checked by the runtime, so the host MUST check the full size.
3. **Given** a rejected capability or pointer check, **When** it is denied, **Then** the device does
   not crash.

---

### User Story 3 - Enforce the GPIO/PWM/ADC/I2C hardware policy (Priority: P2)

Hardware access is bounded by a firmware hard block list and a per-pin manifest whitelist; pins
wired to critical subsystems can never be claimed and conflicting claims are rejected as busy.

**Why this priority**: Exposing reserved pins (display SPI, TROPIC01, charger, USB, PSRAM, flash,
octal-PSRAM data lines) to a plugin can crash or compromise the badge; the policy is a hard safety
boundary.

**Independent Test**: From a plugin, request a hard-blocked pin (rejected) and a whitelisted pin
already claimed by another plugin (rejected busy), and confirm a properly declared free pin succeeds.

**Acceptance Scenarios**:

1. **Given** a plugin requesting a hardware pin on the firmware hard block list (display SPI,
   TROPIC01, charger, USB, PSRAM, flash, octal-PSRAM data lines), **When** it calls the GPIO/PWM/ADC/
   I2C host function, **Then** the call is rejected.
2. **Given** a plugin requesting a pin not in its manifest whitelist, **When** it calls the host
   function, **Then** the call is rejected.
3. **Given** a pin already claimed by another plugin, **When** a second plugin claims the same pin,
   **Then** the conflicting claim is rejected as busy.

---

### User Story 4 - Background and autoload lifecycle (Priority: P3)

A plugin may declare lifecycle flags so it survives after the user leaves its view, starts headless
at boot, or holds a sleep inhibitor while loaded.

**Why this priority**: Residency flags enable always-on plugins (e.g. a messenger or status widget);
they extend, but do not gate, the core runtime.

**Independent Test**: Run a plugin declaring `background` and leave its view (it survives); reboot
with an `autoload` plugin (it starts headless); load a `prevent_sleep` plugin and confirm the sleep
inhibitor is held.

**Acceptance Scenarios**:

1. **Given** a plugin declaring `background`, **When** the holder leaves its view, **Then** the
   plugin survives in the background instead of being unloaded.
2. **Given** a plugin declaring `autoload`, **When** the badge boots, **Then** the plugin starts as a
   resident background instance headless (no foreground view).
3. **Given** a plugin declaring `prevent_sleep`, **When** it is loaded, **Then** a sleep inhibitor is
   held while the plugin is loaded.

---

### Edge Cases

- **Incompatible host-API level**: a plugin requiring a different major, or a higher minor than the
  firmware, is rejected.
- **Out-of-bounds pointer**: a bare `*` host argument is only 1-byte-checked by the runtime, so the
  host MUST validate the full extent; an over-long pointer/length is rejected without a crash.
- **Blocked / undeclared / busy pin**: hard-blocked pins, undeclared pins, and already-claimed pins
  are all rejected.
- **SDK drift**: the canonical `host_api.h` and the plugin SDK copy must stay byte-identical; any
  surface change is committed in both repos together (Constitution V).
- **Plugin lifecycle details**: [NEEDS CLARIFICATION: implementation details documented only at a
  high level — plugin foreground→background demotion timing, plugin file-sandbox enforcement point,
  and plugin file overwrite atomicity. (baseline B15)]

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-071**: The device MUST run third-party plugins as WebAssembly inside a WAMR runtime
  (interpreter; AOT loader present but native AOT disabled by default), with linear memory in PSRAM
  (16–4096 KB, default 64 KB).
- **FR-072**: Plugins MUST be gated by a manifest capability model; host functions MUST validate
  capabilities and re-validate plugin-supplied pointers/lengths against linear memory (a bare pointer
  is only 1-byte-checked by the runtime, so the host MUST check the full extent).
- **FR-073**: GPIO/PWM/ADC/I2C access MUST honour a firmware hard block list (display SPI, TROPIC01,
  charger, USB, PSRAM, flash, octal-PSRAM data lines) and a per-pin manifest whitelist; conflicting
  claims MUST be rejected as busy.
- **FR-074**: The host API MUST be versioned (major must match firmware; minor ≤ firmware minor); the
  canonical `host_api.h` MUST stay byte-identical to the plugin SDK copy.
- **FR-075**: A plugin MAY declare `background` (survive after the user leaves its view) and/or
  `autoload` (start headless at boot) and/or `prevent_sleep` (hold a sleep inhibitor while loaded).
  [NEEDS CLARIFICATION: foreground→background demotion timing, file-sandbox enforcement point, and
  file overwrite atomicity are documented only at a high level. (baseline B15)]

### Key Entities *(include if feature involves data)*

- **Plugin** — `<id>.wasm`/`.aot` + `.meta` manifest (plus optional `.lang`, `.disabled` marker) on
  the plugins partition; declares capabilities, linear memory size, prerequisites and lifecycle
  flags. Uses the plugin pool: ECC slot 31 + R-Memory slots 501–511.
- **Manifest Capability Model** — the declared set of capabilities and per-pin GPIO/PWM/ADC
  whitelists in the manifest that each host call is checked against at load time and per call.
- **GPIO Policy** — the firmware hard block list (display SPI, TROPIC01, charger, USB, PSRAM, flash,
  octal-PSRAM data lines GPIO 33–37) combined with the per-pin manifest whitelist and a per-pin lock
  table; conflicting claims are rejected as busy.
- **Host API** — the versioned `host_*` surface (major must match firmware, minor ≤ firmware minor);
  canonical `host_api.h` in this repo, byte-identical to the plugin SDK copy.
- **Linear Memory** — the plugin's WebAssembly linear memory, PSRAM-resident, 16–4096 KB (default
  64 KB).
- **Lifecycle Flags** — `background`, `autoload`, `prevent_sleep`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A plugin that requests a blocked pin, an undeclared capability, or out-of-bounds memory
  is denied 100% of the time without crashing the device.
- **SC-002**: A plugin with a compatible host-API level (major equal, minor ≤ firmware) starts and
  presents its UI; one with a mismatched major or higher minor is rejected 100% of the time.
- **SC-003**: A plugin's linear memory is PSRAM-resident within the 16–4096 KB bound (default 64 KB),
  and native AOT is disabled by default.
- **SC-004**: Lifecycle flags behave as declared: a `background` plugin survives leaving its view, an
  `autoload` plugin starts headless at boot, and a `prevent_sleep` plugin holds a sleep inhibitor
  while loaded.
- **SC-005**: The canonical `host_api.h` and the plugin SDK copy are byte-identical (no surface
  drift).

## Assumptions

- The plugin sandbox is a NON-NEGOTIABLE trust boundary (Constitution III); the GPIO hard block list
  and full-extent pointer validation must never be weakened for convenience.
- Native AOT is disabled by default (`FEATURE_PLUGIN_AOT=0` in a release build, per baseline SC-013);
  this spec does not assert a target version or toggle it.
- The canonical `host_api.h` lives in this repo and the cdc-badge-plugins SDK copy is only ever
  `cp`-ed over, never hand-edited; any surface change ships in both repos together (Constitution V).
- The self-destruct/factory-wipe mechanics (FR-070) and the plugin pool's secure-element slots are
  owned by spec `013-persistence-factory-reset`; this spec references the pool but does not own it.
- The lifecycle implementation details (foreground→background demotion timing, file-sandbox
  enforcement point, file overwrite atomicity — baseline B15) are open items to be resolved by
  reading the code, then documented; they are recorded here rather than guessed.
- Host-tier tests can cover the capability/GPIO policy (hard block list + manifest whitelist + busy
  conflicts — T-H08); the runtime sandbox (blocked pin / undeclared capability / OOB pointer rejected
  without crash) is verified on hardware by HIL plan T-HIL07.
