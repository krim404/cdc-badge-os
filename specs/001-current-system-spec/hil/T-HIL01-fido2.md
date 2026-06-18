# T-HIL01 — FIDO2 / WebAuthn register + assert over USB

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-010, FR-011, FR-012, FR-013, FR-014, FR-015, FR-016, FR-017

Verifies the badge as a CTAP2 authenticator over USB HID: a WebAuthn credential can be
registered and used for assertion with on-device user presence, the ClientPIN flow uses the
badge PIN hash `LEFT(SHA-256(PIN), 16)`, and the per-credential signature counter increments on
each successful assertion. Maps to Success Criterion SC-003.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1 with a working TROPIC01 secure element.
- USB-C cable to the host. No BLE peer required.

### Host tools
- A host OS with a CTAP2 stack. Either:
  - A browser-based WebAuthn relying party (e.g. `https://webauthn.io`), or
  - `python -m fido2` / `fido2-token` from the `python-fido2` package for scripted CTAP2.
- Optional: serial terminal at 115200 baud on the badge CDC port (`/dev/cu.usbmodem*`,
  enumerates as `BadgeV1`) for log inspection.

### Build profile
- Any beta build (firmware < 1.0) is acceptable; `DEBUG_MODE` may be on.
- Badge PIN must be known. Dev device PIN is `0000`; this is also the FIDO2 ClientPIN
  (FR-017: same secret).

## Procedure

1. Plug the badge into the host over USB. Confirm it enumerates a FIDO HID interface
   (CTAPHID, 64-byte reports). On serial, confirm the FIDO2 module started without a
   secure-element slot error.
2. **getInfo (FR-010/FR-011)**: query the authenticator info.
   - `fido2-token -I <device>` (or browser DevTools / `webauthn.io` capability probe).
   - Record advertised versions and options.
3. **Register without ClientPIN (FR-012/FR-013)**: at a relying party that does not require
   user verification, start credential registration.
   - On the badge confirm the "Register Key" prompt shows the relying-party name.
   - Press `Y` (confirm) within the prompt window.
   - Confirm the host receives a credential and registration succeeds.
4. **Overwrite warning (acceptance scenario 2)**: re-register a credential for the **same**
   relying party. Confirm the badge warns about overwriting before proceeding; cancel with `N`
   and confirm no new credential was created, then repeat and confirm with `Y`.
5. **Assertion + counter (FR-014)**: sign in at the same relying party.
   - Confirm presence on the badge with `Y`.
   - Confirm the host accepts the assertion.
   - Repeat the sign-in a second and third time, confirming presence each time.
   - Capture the WebAuthn `signCount` (authenticator data signature counter) on the relying
     party / via the host tool for each of the three assertions.
6. **ClientPIN verify uses LEFT(SHA-256,16) (FR-017)**: at a relying party that requires user
   verification (residentKey + userVerification=required), run the ClientPIN flow.
   - Enter the badge PIN when prompted by the host's ClientPIN dialog.
   - Confirm verification succeeds with the correct PIN.
   - Enter a wrong PIN once and confirm the host reports a PIN-invalid / retries-remaining
     error (do NOT exhaust attempts — see Notes).
7. **Selection probe (FR-013)**: trigger a device-selection probe (CTAP `selection`), e.g. a
   parallel-authenticator picker in the browser. Confirm the badge shows a device-selection
   prompt requiring user presence only (no PIN entry).
8. **Attestation (FR-015)**: inspect the registration attestation object from step 3.
   - Confirm `fmt` is `packed`, the attestation is self-signed, and the AAGUID equals
     `CDCBAD6E39C30001BAD6E00100000001`.

## Pass criteria

- Step 2: getInfo advertises `FIDO_2_0`, `FIDO_2_1` and `U2F_V2`; options include `rk`, `up`,
  `clientPin`, `credMgmt`, `pinUvAuthToken`; `uv` is absent/false; `largeBlobs`,
  `authenticatorConfig` and `bioEnroll` are absent/unsupported.
- Step 3: registration completes after exactly one on-device `Y` confirmation within the prompt
  window; the host stores a credential with a 64-byte credential ID.
- Step 4: a second registration for the same RP shows an overwrite warning; `N` aborts with no
  credential change, `Y` proceeds.
- Step 5: each of the three assertions completes with one on-device confirmation, and the
  reported `signCount` is **strictly greater** on each successive assertion (monotonic increment,
  e.g. n < n+1 < n+2).
- Step 6: correct PIN verifies; a wrong PIN is rejected by the ClientPIN compare. (The shared
  secret with the badge PIN is asserted by FR-017; the observable here is that the same digits
  that unlock the badge also satisfy ClientPIN.)
- Step 7: the selection probe shows a device-selection prompt and completes on user presence
  alone, never prompting for a PIN.
- Step 8: attestation `fmt == "packed"`, self-signed, AAGUID == `CDCBAD6E39C30001BAD6E00100000001`.

## Notes

- **credProtect (FR-016, open item B2)**: `credProtect` levels are parsed/stored/reported but NOT
  enforced at assertion time. Do not treat non-enforcement as a failure of this plan; record it
  as an observation against B2.
- **No flashing**: this plan runs against the installed release. Run all steps against the one
  flashed image.
- **ClientPIN lockout caution**: do not enter a wrong ClientPIN repeatedly. ClientPIN shares the
  badge-PIN lockout (FR-002/FR-017); too many wrong entries trigger the 60-second lockout. One
  deliberate wrong entry in step 6 is sufficient.
- Wait a few seconds after any re-enumeration before issuing host CTAP2 commands.
