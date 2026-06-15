# T-HIL07 — Plugin sandbox: blocked pin / undeclared capability / OOB pointer rejected, no crash

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-072, FR-073

Verifies the WAMR plugin sandbox: a plugin requesting a hardware pin on the firmware hard block
list, a capability not declared in its manifest, or supplying an out-of-bounds pointer/length to
a host function is **rejected without crashing the device**. Maps to Success Criterion SC-010 and
NFR-005.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1.
- USB-C cable to the host.
- Optional: a multimeter/logic probe to confirm a blocked pin never toggles (defense-in-depth
  observation; the host-API rejection is the primary signal).

### Host tools
- `tools/upload.py` (serial plugin upload) and a serial terminal at 115200 baud.
- Rust plugin toolchain (rustup) to build the probe plugins, or pre-built test `.wasm`/`.meta`.
- `tools/coredump.py` (only if an unexpected crash needs analysis — a crash would be a failure).

### Build profile
- Any beta build with the plugin runtime enabled. Plugins partition (`/plugins`) mounted.
- Badge PIN known (dev: `0000`) for the serial `AUTH` gate if active.

### Test plugins (three small probes)
Build three minimal plugins, each attempting one violation. Do **not** modify firmware; these
live only in the plugin repo / as uploaded artifacts:
- **P-pin**: manifest declares NO gpio pins; calls `host_gpio_*` on a hard-block pin
  (e.g. GPIO 19, a USB line) and also on an octal-PSRAM data line (e.g. GPIO 33).
- **P-cap**: manifest omits a capability (e.g. no `secure_element`/`storage` capability) and calls
  the corresponding undeclared host function.
- **P-oob**: passes a pointer/length to a host function that points outside the plugin's linear
  memory (and a bare `*` arg whose full extent exceeds the buffer — exercising the host's
  full-extent re-validation, not the runtime's 1-byte check).

## Procedure

1. **Upload + start P-pin**: `tools/upload.py` the P-pin plugin, then `PLUGIN START <id>` (or
   start from the Plugins menu). When it calls `host_gpio_*` on the blocked pin:
   - Confirm the host function returns a rejection (e.g. `HOST_ERR_BUSY` / capability error) and
     the pin is never driven.
   - Confirm the badge stays responsive (UI/serial alive); no reboot, no crash.
2. **Octal-PSRAM line (FR-073)**: confirm P-pin's attempt on a GPIO 33–37 octal-PSRAM data line is
   blocked (these must stay BLOCKED — driving them crashes the badge if it leaked through).
3. **Upload + start P-cap (FR-072)**: start P-cap; when it calls the undeclared-capability host
   function, confirm the call is rejected and the plugin does not gain the capability. Badge
   stays responsive.
4. **Upload + start P-oob (FR-072)**: start P-oob; when it passes an out-of-bounds
   pointer/length, confirm the host rejects the call (full-extent bounds check) rather than
   reading/writing outside linear memory. Badge stays responsive.
5. **Stability sweep**: after each probe, navigate the badge UI and issue a serial command (e.g.
   `PING` / `PLUGIN LIST`) to confirm the device is still alive and the runtime did not wedge.
6. **Clean up**: `PLUGIN STOP` and `PLUGIN DELETE` each probe plugin.

## Pass criteria

- Step 1: a host GPIO call on a hard-block pin is rejected; the pin is never asserted; no crash.
- Step 2: a GPIO call on a GPIO 33–37 octal-PSRAM line is rejected and BLOCKED; no crash.
- Step 3: an undeclared-capability host call is rejected; the plugin does not obtain the
  capability; no crash.
- Step 4: an out-of-bounds pointer/length is rejected by the host's full-extent re-validation; no
  OOB read/write occurs; no crash.
- Across all steps: each violation is denied **100% of the time without crashing the device**
  (SC-010); the badge remains responsive on UI and serial after every probe.

## Notes

- **Non-blocking**: this is a non-blocking HIL plan; failures are recorded against the plugin
  sandbox spec, not a build gate. A **crash/reboot** on any probe is a hard failure (the whole
  point is rejection without crashing) — capture a coredump (`tools/coredump.py`) if it happens.
- **Flash conservation**: requires **no firmware flashing** — only plugin uploads over serial
  (`tools/upload.py`). Upload only the changed probe plugins; do not re-upload all plugins per
  iteration.
- **Hard block list reference** (firmware-enforced, mirrored in `GpioSerialCommands.cpp`): GPIO
  0, 1, 8, 10–13, 17–21, 26–32, 39, 41, 42, 45–48 are blocked (Display SPI, TROPIC01, charger,
  USB, PSRAM, flash); GPIO 33–37 are octal-PSRAM data lines and MUST stay blocked. Pick blocked
  pins for the probes from these ranges.
- Do not whitelist a blocked pin in the manifest to "make it pass" — the firmware hard block list
  overrides the manifest whitelist by design.
