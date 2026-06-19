# Feature Specification: CDC Badge OS — Current System Baseline

**Feature Branch**: `message-transfer-framework` (spec directory `001-current-system-spec`)

**Created**: 2026-06-14

**Status**: Draft (reverse-specification of the existing system)

**Input**: "Erstelle eine Spezifikation für das bestehende Repository. Ziel ist keine neue
Funktion, sondern eine präzise Beschreibung des aktuellen Systems." — analyse purpose,
user flows, domain objects, external interfaces, data flows, authn/authz, persistence,
error cases, non-functional requirements, implicit assumptions; mark unclear points as
NEEDS CLARIFICATION; invent no requirements.

> **Nature of this document**: This is a *reverse-specification* — it describes the system
> as it currently exists. The in-repo documentation under `website/src/content/docs/` is the
> **intended** long-term source of truth, but it was just generated and is **not yet reconciled
> with the code**. Therefore, for this as-built audit the **code is the ground truth for current
> behaviour**, and every doc-vs-code mismatch is a *documentation defect to fix* so the docs can
> resume their authoritative role. Statements here are grounded in the code
> (`main/tropic_slot_map.h`, `components/.../feature_flags.h`, `components/.../PinManager.*`,
> `partitions.csv`, `platformio.ini`, `sdkconfig.defaults`) and corroborated by the docs only
> where they agree. It does **not** propose new functionality. Confirmed doc-vs-code mismatches
> are recorded in *Documentation Discrepancies* below (these drive the documentation update);
> genuinely undocumented or hardware-unverified points are marked **[NEEDS CLARIFICATION]**. The
> number of markers intentionally exceeds the usual cap because the task is an as-built audit.

---

## System Overview & Purpose

CDC Badge OS is the firmware for the **CDC Badge v1.0/v1.1**, a self-contained hardware
security key built on an ESP32-S3 paired with a **TROPIC01 secure element**, a 2.9" monochrome
e-paper display (296×128, frontlit) and a 12-button keypad. It is operated standalone (no
companion app required for core functions) and exposes cryptographic services to connected
hosts.

**Actors**:

- **Badge holder** — owns and physically operates the device via keypad + display.
- **Host computer** — connects over USB; consumes FIDO2/WebAuthn, OpenPGP smartcard (CCID),
  USB-keyboard auto-type, Yubico-style OTP challenge-response, and the serial console.
- **Companion phone / host** — optionally connects over BLE (HID keyboard, serial console).
- **Peer badge** — another CDC Badge for badge-to-badge exchange (vCard, GPG key cross-signing)
  over BLE.
- **Plugin** — sandboxed third-party WebAssembly code running inside the device.

**Value proposition**: One device holds and uses cryptographic credentials (passkeys, GPG/SSH
keys, TOTP/HOTP seeds, a password vault) with private keys generated on-chip and never
exported, extensible via a sandboxed plugin runtime, with on-device confirmation for sensitive
operations.

---

## Clarifications

### Session 2026-06-14

- Q: How should WIP / hardware-unverified features be scoped in this as-built spec? → A: In-scope,
  flagged "Provisional (WIP, not hardware-verified)"; their HIL acceptance is non-blocking until
  verified on hardware.
- **Provisional (WIP) scope**: FR-044 (GPG cross-sign send path) remains normative-but-Provisional
  (not hardware-verified), as does the BLE serial console. FR-050..054 (cdc_msg badge-to-badge
  messaging, incl. BLE vCard exchange) and BLE HID auto-type were **hardware-verified (2026-06-18)**.
- Q: What build-profile constraints define a valid production/release build? → A: Release MUST have
  `DEBUG_MODE=0`, `FEATURE_SECURE_SERIAL=1`, `FEATURE_PLUGIN_AOT=0`, `FEATURE_NVS_EDIT=0` (measurable
  release gate; see SC-013).
- Q: What counts as a "release" build for SC-013? → A: A build with firmware version ≥ 1.0;
  everything before 1.0 is **beta** and is NOT bound by the release gate (`DEBUG_MODE` may remain on
  during beta).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Unlock and protect the device (Priority: P1)

The badge holder protects all on-device secrets behind a numeric PIN. The device boots to a
lock screen; the holder must enter the correct PIN to reach any feature. Brute-force entry is
rate-limited, and an optional duress PIN can wipe the device.

**Why this priority**: PIN unlock is the gate to every other capability; without it no other
flow is reachable.

**Independent Test**: Boot a provisioned badge, confirm it shows the lock screen, enter the
correct PIN, and confirm the main menu opens. Enter a wrong PIN until the attempt budget is
exhausted (one attempt immediately after a cold boot) and confirm the lockout behaviour.

**Acceptance Scenarios**:

1. **Given** a provisioned badge at the lock screen, **When** the holder presses a key (other
   than the menu key) and enters the correct PIN followed by confirm, **Then** the main menu
   opens.
2. **Given** the lock screen, **When** the holder enters a wrong PIN and the attempt budget is
   exhausted (one attempt immediately after a cold boot; up to three within a recovery window),
   **Then** the device enters a 60-second lockout with a visible countdown and refuses further attempts
   until it expires, after which entry is allowed again without permanent bricking.
3. **Given** a configured duress PIN, **When** that duress PIN is entered at unlock, **Then**
   the device behaves indistinguishably from a normal failed attempt while initiating a full
   wipe of all on-chip secrets on the next boot.
4. **Given** an unlocked session, **When** 5 minutes of inactivity elapse, **Then** the device
   returns to the lock screen.

---

### User Story 2 - Use the badge as a FIDO2/WebAuthn authenticator (Priority: P1)

The host computer registers and authenticates passkeys against the badge over USB; the badge
generates keys on-chip and requires on-device user presence for each operation.

**Why this priority**: FIDO2/WebAuthn is the primary reason the device exists as a security key.

**Independent Test**: From a WebAuthn relying party, register a credential and confirm it on the
badge; then sign in and confirm the assertion succeeds.

**Acceptance Scenarios**:

1. **Given** a host initiating WebAuthn registration, **When** the badge shows the
   "Register Key" prompt with the relying-party name and the holder presses confirm within the
   prompt window, **Then** an on-chip key pair is generated and a credential is returned to the
   host.
2. **Given** an existing credential for the same relying party, **When** a new registration is
   requested, **Then** the badge warns about overwriting before proceeding.
3. **Given** a registered credential, **When** the host requests an assertion and the holder
   confirms presence, **Then** the badge returns a signed assertion and the per-credential
   signature counter increments.
4. **Given** a relying party that requires user verification, **When** the WebAuthn ClientPIN
   flow runs, **Then** the badge enforces PIN verification according to CTAP2 ClientPIN v2.

---

### User Story 3 - Generate and use one-time passwords (TOTP/HOTP/CR) (Priority: P2)

The holder stores OATH secrets and reads time- or counter-based one-time passwords on the
display, or answers HMAC challenge-response over USB/serial/BLE.

**Independent Test**: Add a TOTP account via the on-device wizard, then view it and confirm a
6-digit code with a countdown is displayed.

**Acceptance Scenarios**:

1. **Given** the 2FA menu, **When** the holder completes the add-account wizard (type, name,
   Base32 secret, digits, algorithm, period), **Then** the account is saved to the secure
   element and confirmed.
2. **Given** a saved TOTP account, **When** the holder opens it, **Then** the current code, a
   progress bar and a per-second countdown are shown.
3. **Given** a saved HOTP account, **When** the holder advances the counter, **Then** a new code
   is shown and the counter persists.
4. **Given** a challenge-response account, **When** a host sends a 64-byte challenge over the
   OTP HID interface (and touch is confirmed if required), **Then** the badge returns a 20-byte
   HMAC-SHA1 response.

---

### User Story 4 - Store and auto-type passwords (Priority: P2)

The holder keeps login credentials in an on-device vault and types them into a connected host
via USB or BLE keyboard emulation.

**Independent Test**: Create a vault entry, connect a HID keyboard target, open the entry, and
confirm the password is typed into the host.

**Acceptance Scenarios**:

1. **Given** the Passwords menu, **When** the holder completes the new-entry wizard
   (title, username, password or generated password, URL, optional TOTP slot, notes), **Then**
   the entry is saved.
2. **Given** a vault entry and a connected HID keyboard (USB or BLE), **When** the holder
   triggers auto-type from the detail view, **Then** the stored field is sent as keystrokes to
   the host.

---

### User Story 5 - Use OpenPGP smartcard for GPG and SSH (Priority: P2)

The holder generates OpenPGP key triples (SIG/DEC/AUT) on-chip and uses the badge as an
OpenPGP CCID smartcard for signing, decryption and SSH authentication.

**Independent Test**: Generate keys on-device, export the public key, plug into a host, and
confirm `gpg --card-status` enumerates the card.

**Acceptance Scenarios**:

1. **Given** the GPG menu, **When** the holder runs key generation (name, optional email,
   curve Ed25519 or P-256), **Then** SIG, DEC and AUT keys are created on-chip (DEC fixed to
   P-256) and never exported.
2. **Given** a host with gpg-agent, **When** the badge is connected over USB, **Then** it is
   enumerated as an OpenPGP CCID smartcard and can sign/decrypt/authenticate gated by the
   OpenPGP User PIN (PW1) / Admin PIN (PW3).

---

### User Story 6 - Exchange data badge-to-badge and back up data (Priority: P3)

The holder shares a vCard with a nearby badge over BLE using numeric-comparison confirmation,
and exports/imports a passphrase-encrypted backup of non-secure-element data.

**Independent Test**: Edit own vCard, send it to a peer badge with numeric-comparison
confirmation on both sides, and confirm the peer stores it. Export an encrypted backup, then
import it and confirm a per-module result tally.

**Acceptance Scenarios**:

1. **Given** two badges with the beacon enabled on the receiver, **When** the sender selects
   "Send vCard", picks the peer, and both confirm the matching six-digit code, **Then** the
   vCard transfers and the receiver stores it (deduplicated by exact text).
2. **Given** the Expert → Backup menu, **When** the holder exports with a non-empty passphrase,
   **Then** an encrypted `backup.cdcbak` is written containing TOTP, password-vault, vCard and
   OS/WiFi settings (never secure-element private keys).
3. **Given** an encrypted backup and the correct passphrase, **When** the holder imports it,
   **Then** records are merged best-effort and a per-section tally (imported/failed/skipped) is
   shown; a wrong passphrase or corrupt file fails with a clear error.

---

### User Story 7 - Extend the device with sandboxed plugins (Priority: P3)

The holder installs WebAssembly plugins without reflashing firmware; plugins run inside a
capability-gated sandbox.

**Independent Test**: Upload a plugin over serial, start it, confirm it runs, and confirm a
denied capability (e.g. a blocked GPIO pin) is rejected.

**Acceptance Scenarios**:

1. **Given** a plugin binary and manifest, **When** it is uploaded to the plugins partition and
   its required host-API level is compatible, **Then** it can be started and presents its UI.
2. **Given** a plugin requesting a hardware pin not in its manifest or on the firmware block
   list, **When** it calls the corresponding host function, **Then** the call is rejected.
3. **Given** a plugin declaring `background`/`autoload`, **When** the holder leaves its view or
   the badge boots, **Then** the plugin survives in the background / starts headless at boot
   respectively.

---

### Edge Cases

- **Wrong PIN under lockout**: further attempts are refused until the 60-second timer expires;
  the retry counter lives in RAM so a power cycle mid-verify cannot corrupt it.
- **OpenPGP PW3 exhaustion**: three wrong Admin-PIN entries terminally block card management
  until a full device wipe (no host-side reset) — a non-recoverable state by design.
- **Interrupted duress wipe**: if power is lost mid-wipe, the boot marker remains absent and the
  wipe re-runs on next boot; the device cannot be left half-wiped.
- **Build-profile mismatch / format change**: a mismatch of the build-profile byte (or a
  breaking on-device format) triggers a full factory wipe + reinit on next boot (no migration).
- **Backup with newer host-API level**: import refuses a backup whose recorded host-API level is
  newer than the running firmware.
- **Secure-element failure**: a module whose secure-element slot is in error fails to start.
- **Plugin upload stalls**: an upload session auto-aborts after 15 seconds of inactivity so a
  crashed client cannot wedge the console.
- **Message-transfer abuse**: inbound transfer prompts are rate-limited (global and
  per-connection budgets, post-decline cooldown); a full offer queue replies Busy.
- **vCard / message too large**: payloads over the framework limit, oversized MIME/name fields,
  or malformed frames are declined.
- **E-paper staleness**: a momentary "stale" look between refreshes is expected; the lock-screen
  clock uses a light partial refresh that is never promoted to a full refresh.

---

## Requirements *(mandatory)*

### Functional Requirements

Grouped by capability area. Each requirement describes observable, as-built behaviour.

#### Authentication & lock state

- **FR-001**: The device MUST boot to a lock screen and require a numeric badge PIN (4–8 digits)
  before exposing any menu or feature.
- **FR-002**: The badge PIN retry budget is 3 attempts, tracked in RAM. On boot the device grants
  1 attempt (0 if it was locked at power-off) and starts a 60-second recovery timer; exhausting the
  budget on a wrong entry locks the device and (re)starts the 60-second timer with a visible
  countdown; on timer expiry the budget is restored to 3 and the locked flag is cleared.
- **FR-003**: The badge PIN MUST be self-recovering: the device MUST NOT be permanently bricked
  by repeated wrong badge-PIN entry, and the lockout recovery MUST behave identically in debug
  and release builds (no bypass).
- **FR-004**: The device MUST support an optional, default-off duress PIN that, when entered,
  initiates a full self-destruct (see FR-070) while being indistinguishable from a failed unlock
  (no UI, log, or timing tell); the duress PIN MUST differ from the badge PIN (bidirectionally
  enforced).
- **FR-005**: The device MUST auto-lock after 5 minutes of inactivity in menus (unless a loaded
  plugin holds a sleep/lock inhibitor).
- **FR-006**: The PIN record MUST be stored in TROPIC01 R-Memory slot 0 and bound to the device
  via an attestation signature; a failed signature check MUST cause a silent reinit to defaults.
  [NEEDS CLARIFICATION: exact attestation signature algorithm/format and hash for the slot-0
  record — docs say "signed by the slot-0 attestation key" without specifying ECDSA/EdDSA.]
- **FR-007**: PIN hashing is **protocol-determined and intentionally not uniform**: the badge PIN
  MUST be stored as `LEFT(SHA-256(PIN), 16)` because it doubles as the FIDO2 CTAP2 ClientPIN hash
  (the platform transmits exactly this 16-byte value at verification and the authenticator never
  sees the plaintext, so it cannot recompute an S2K hash); OpenPGP PW1/PW3 MUST use Iterated+Salted
  S2K over SHA-256 because the card advertises a KDF-DO and the host transmits the S2K hash; the
  duress PIN (internal only) also uses S2K. *(Code-confirmed: `PinManager::computeBadgeHash` vs
  `computeKdfHash`; FIDO2 bridge `pin_storage_{get,verify}_fido2_hash` → `ctap2.cpp` ClientPIN
  compare at 2648/2884. Unifying the two KDFs is NOT possible without breaking FIDO2 or the OpenPGP
  card — see Discrepancy D2.)*

#### FIDO2 / WebAuthn (USB HID CTAPHID)

- **FR-010**: The device MUST act as a CTAP2 authenticator over USB HID, advertising FIDO 2.0,
  FIDO 2.1 and U2F V2, with capabilities including resident keys (`rk`), user presence (`up`),
  client PIN (`clientPin`), credential management (`credMgmt`) and `pinUvAuthToken`; built-in
  user verification (`uv`) is not provided.
- **FR-011**: The device MUST implement the CTAP2 commands makeCredential, getAssertion, getInfo,
  clientPIN, reset, getNextAssertion, credentialManagement and selection; largeBlobs,
  authenticatorConfig and bioEnrollment MUST report unsupported.
- **FR-012**: Credential private keys MUST be generated on-chip (ES256/P-256 or EdDSA/Ed25519)
  and stored in TROPIC01 ECC slots; the device MUST support up to 26 credentials (ECC slots
  5–30) with 64-byte credential IDs.
- **FR-013**: Each FIDO2 operation MUST require on-device user-presence confirmation; a device-
  selection probe (CTAP `selection`) MUST require user presence only (no PIN).
- **FR-014**: Per-credential monotonic signature counters MUST persist in the secure element and
  increment on each assertion.
- **FR-015**: Attestation MUST use the `packed`, self-signed, per-device format with the chip-
  bound ECC slot 0 key and a fixed AAGUID (`CDCBAD6E39C30001BAD6E00100000001`).
- **FR-016**: [NEEDS CLARIFICATION: `credProtect` levels 1–3 are parsed/stored/reported but NOT
  enforced at assertion time (documented gap). Is this the intended end state or a known defect?]
- **FR-017**: The FIDO2 ClientPIN and the badge PIN MUST be the **same secret**: the FIDO2
  ClientPIN hash is the badge PIN hash `LEFT(SHA-256(PIN),16)`, shared via the
  `pin_storage_{get,verify}_fido2_hash` bridge. *(Code-confirmed; resolves former open question B1.
  This sharing is also why the badge PIN hashing is fixed by CTAP2 — see FR-007/D2.)*

#### One-time passwords (2FA module)

- **FR-020**: The device MUST support TOTP, HOTP and HMAC challenge-response (CR) accounts,
  configured via an on-device wizard and via serial commands, stored in secure-element R-Memory
  (slots 32–131, up to 100 accounts shared across the three types).
- **FR-021**: TOTP accounts MUST support SHA1/SHA256/SHA512, 6/7/8 digits and a configurable
  period; the view MUST show the current code with a progress bar and countdown.
- **FR-022**: HOTP accounts MUST persist a counter that advances on use.
- **FR-023**: CR accounts MUST answer a 64-byte challenge with an HMAC digest matching the account's
  algorithm (20-byte HMAC-SHA1 or 32-byte HMAC-SHA256) over serial or BLE; the USB OTP HID (Yubico
  slot-2) interface returns a 20-byte HMAC-SHA1 response and accepts SHA1 accounts only. Optional
  per-entry touch confirmation applies.

#### Password vault

- **FR-030**: The device MUST store password entries (title, optional username, optional
  password with on-device random generation, optional URL, optional linked TOTP slot, optional
  notes) in secure-element R-Memory (slots 132–500, up to 369 entries).
- **FR-031**: The device MUST type a selected entry's field to a connected USB or BLE HID
  keyboard on holder confirmation.
- **FR-032**: [NEEDS CLARIFICATION: at-rest protection of vault entries within R-Memory — the
  encryption scheme (per-entry vs container, cipher, key) is not documented; one source notes the
  payload is written with header + checksum without an explicit application-level cipher.]

#### OpenPGP smartcard (USB CCID)

- **FR-040**: The device MUST present an OpenPGP CCID smartcard (T=1) over USB and implement the
  OpenPGP card APDU set (SELECT, GET/PUT DATA, VERIFY, CHANGE/RESET reference data, PSO
  SIGN/DECIPHER, INTERNAL AUTHENTICATE, GENERATE ASYMMETRIC KEY PAIR, GET CHALLENGE, etc.).
- **FR-041**: It MUST hold three key roles — SIG (ECC slot 1), AUT (ECC slot 3), both Ed25519 by
  default and switchable to P-256, and DEC (fixed P-256 ECDH, software key encrypted at rest in
  R-Memory with AES-256-GCM).
- **FR-042**: Card operations MUST be gated by PW1 (user, default `123456`, min 6) and PW3
  (admin, default `12345678`, min 8); each allows 3 attempts; PW1 is admin-resettable, PW3 is
  terminal (wipe-only recovery).
- **FR-043**: The device MUST support generating keys on-chip and exporting only the public key
  (PEM to serial + QR on display); private keys MUST never be exported.
- **FR-044**: The device MUST support receiving a peer badge's public key over BLE and producing
  an RFC 4880 certification signature (cross-signing), exportable as an armored public-key block
  over serial. [NEEDS CLARIFICATION: the on-device "Send Key" action is a placeholder; the BLE
  send path exists but is not UI-driven. Confirm intended scope.] [NEEDS CLARIFICATION: cross-sign
  signatures always label the curve as EdDSA from the status snapshot, so P-256 signatures may be
  mislabeled and are unverified on hardware.]

#### Badge-to-badge messaging (cdc_msg framework)

- **FR-050**: The device MUST provide a generic MIME-typed badge-to-badge transfer over BLE GATT
  (Control/Status/Data characteristics) with chunked framing and CRC32 completion check.
- **FR-051**: Inbound transfers MUST be routed to a registered handler by MIME type; with no
  handler the transfer MUST be declined.
- **FR-052**: Transfers MUST require ephemeral numeric-comparison pairing confirmed on both
  badges; the bond MUST be forgotten after the transfer.
- **FR-053**: The framework MUST enforce limits: payload ≤ 4096 bytes, MIME ≤ 63 bytes, peer name
  ≤ 31 bytes, ≤ 8 registered handlers, ≤ 4 queued offers; plus abuse-resistance budgets (global
  ~5 prompts/30s, per-connection ~3/10s, post-decline cooldown).
- **FR-054**: The vCard module MUST register a `text/vcard` handler and provide "Send vCard";
  received vCards MUST be deduplicated by exact text. (vCard exchange and the message-transfer
  framework were hardware-verified 2026-06-18.)

#### Encrypted backup / restore

- **FR-060**: The device MUST export a passphrase-encrypted backup container (`backup.cdcbak`,
  base64 text on the plugins partition) covering 2FA accounts, password-vault entries, vCards and
  system settings (language, display, sleep, timezone, badge text, WiFi credentials, module
  enable state).
- **FR-061**: The backup MUST be encrypted with AES-256-GCM using a key derived via
  PBKDF2-HMAC-SHA256 (200,000 iterations, 16-byte random salt), with the container header as AAD;
  the binary layout is magic `CDCBAK` ‖ version ‖ kdf_iters ‖ salt ‖ nonce ‖ ciphertext ‖ tag.
- **FR-062**: Secure-element private keys (FIDO2, GPG/SSH) MUST NOT be included in any backup.
- **FR-063**: Import MUST be best-effort: upsert by identity (TOTP by account name, password by
  title, vCard by exact text), skip unknown modules / mismatched schema versions, refuse a backup
  with a newer host-API level, and report aggregate counts.
- **FR-064**: [NEEDS CLARIFICATION: whether backup export requires an unlocked device / badge PIN
  in addition to the passphrase is not documented.]

#### Plugins (WAMR sandbox)

- **FR-070** *(self-destruct, referenced by FR-004)*: On duress trigger the device MUST erase the
  NVS boot marker and reboot; on the next boot it MUST run a full factory wipe — NVS reinit, all
  TROPIC01 ECC slots 0–31 deleted, all R-Memory slots 0–511 erased, attestation key regenerated.
  Firmware image and off-badge exports are out of scope of the wipe.
- **FR-071**: The device MUST run third-party plugins as WebAssembly inside a WAMR runtime
  (interpreter; AOT loader present but native AOT disabled by default), with linear memory in
  PSRAM (16–4096 KB, default 64 KB).
- **FR-072**: Plugins MUST be gated by a manifest capability model; host functions MUST validate
  capabilities and re-validate plugin-supplied pointers/lengths against linear memory (a bare
  pointer is only 1-byte-checked by the runtime, so the host MUST check the full extent).
- **FR-073**: GPIO/PWM/ADC/I2C access MUST honour a firmware hard block list (display SPI,
  TROPIC01, charger, USB, PSRAM, flash, octal-PSRAM data lines) and a per-pin manifest whitelist;
  conflicting claims MUST be rejected as busy.
- **FR-074**: The host API MUST be versioned (major must match firmware; minor ≤ firmware minor);
  the canonical `host_api.h` MUST stay byte-identical to the plugin SDK copy.
- **FR-075**: A plugin MAY declare `background` (survive after the user leaves its view) and/or
  `autoload` (start headless at boot) and/or `prevent_sleep` (hold a sleep inhibitor while
  loaded).

#### Serial console & host tooling

- **FR-080**: The device MUST expose a line-oriented serial console over USB CDC (115200 baud)
  and over BLE (Nordic UART), with command families for system, time, display, storage (NVS),
  PIN, secure element (TR01), WiFi, modules, GPG, 2FA, password, vCard, backup, plugin/file
  management (PLUGIN/VFAT), i18n, and GPIO/ADC/I2C/SAO.
- **FR-081**: When the secure-serial gate is active, all commands except `PING` and `AUTH` MUST
  require a prior `AUTH <pin>` using the badge PIN, sharing lockout state with the lock screen,
  with a session idle timeout. *(Discrepancy D1: `feature_flags.h` defaults `FEATURE_SECURE_SERIAL`
  to 0/off unless Kconfig `CONFIG_SECURE_SERIAL` is set, but `serial-console.md` states it is
  "enabled by default" — the documentation must be corrected to match the code.)* [NEEDS
  CLARIFICATION: exact AUTH session idle timeout value (read from source); and the Kconfig default
  for `CONFIG_SECURE_SERIAL`, which determines the shipped default.]
- **FR-082**: Plugin/file uploads MUST switch to a raw byte-streaming mode with a declared size
  and CRC-32 verification, and auto-abort after 15 seconds of inactivity.

#### Connectivity, time, settings, power

- **FR-090**: BLE MUST operate as a peripheral with a single connection at a time; pairing for
  host HID uses numeric comparison and the device MAY bond up to 5 host devices. [NEEDS
  CLARIFICATION: behaviour when bonding a 6th device / bond eviction policy.]
- **FR-091**: The device MUST support WiFi setup (scan or manual SSID, encryption selection,
  DHCP or static IP) and NTP time sync (pool.ntp.org, time.google.com), remembering WiFi intent
  across reboot. [NEEDS CLARIFICATION: exact WiFi scan/connect/NTP timeout values — docs say
  "about 10s" / "15s" / "10s".]
- **FR-092**: The Settings menu MUST expose a fixed ordered set: brightness, language, timezone,
  sleep interval, badge text (3 lines), date, time, change PIN.
- **FR-093**: The device MUST support light sleep (idle ~2 min on lock screen, fast wake) and
  deep sleep (hold the back key ~5 s), and display battery level, charging state and status icons
  on the lock screen.
- **FR-094**: The display pipeline MUST render CP437 text and support FULL / PARTIAL /
  PARTIAL_LIGHT refresh modes, where the lock-screen clock uses PARTIAL_LIGHT and is never
  promoted to a full refresh.

#### Internationalisation

- **FR-100**: English MUST be the always-available in-firmware fallback language; additional
  languages MUST be loadable as flat JSON overlays (`/plugins/i18n/lang_<code>.json`) parsed into
  PSRAM, labelled by each file's `core.lang_name` endonym, with UTF-8 → CP437 conversion at parse
  time and per-key fallback to English.

#### Keypad / UI interaction model

- **FR-110**: Navigation MUST follow a consistent 12-key model: 2/8 move selection (hold to jump
  to first/last), Y confirm/select, N back/cancel/delete, 3 opens a context menu; text entry uses
  T9 multi-tap; sliders use 4/6; date/time and PIN use digit entry.
- **FR-111**: A rescue chord (hold N+Y) MUST force the device back to a clean locked state
  (unload plugins, dismiss dialogs) without a hardware reset.

### Key Entities (Domain Objects)

- **Badge PIN** — numeric unlock secret (4–8 digits, default `123456`), retry counter (RAM) +
  persisted locked flag, in R-Memory slot 0, attestation-signed.
- **Duress PIN** — optional alternate PIN (default off) that triggers full self-destruct.
- **FIDO2 Credential** — on-chip key pair (ES256/EdDSA), credential ID (64 B), relying-party,
  user handle, resident flag, per-credential signature counter; ECC slots 5–30 (≤ 26).
- **TOTP / HOTP / CR Account** — OATH secret (Base32), name, issuer, algorithm, digits, period or
  counter, flags; R-Memory slots 32–131 (≤ 100 total).
- **Password Vault Entry** — title, username, password, URL, linked TOTP slot, notes; R-Memory
  slots 132–500 (≤ 369).
- **OpenPGP Key Set** — SIG/DEC/AUT roles, fingerprints (v4), cardholder data, signature counter;
  ECC slots 1–3 + R-Memory 1–3; PW1/PW3 smartcard PINs.
- **Received Peer Key (cross-signing)** — peer curve, public key, v4 fingerprint, user-id, receive
  timestamp, optional certification signature, verified flag; NVS-backed, ≤ 128 keys.
- **vCard / Contact** — vCard 4.0 record (≤ 768 B): own card + received cards (≤ 100).
- **Message Transfer** — ephemeral MIME-typed payload (≤ 4096 B) with sender identity and
  numeric-comparison code.
- **Plugin** — `<id>.wasm`/`.aot` + `.meta` manifest (+ optional `.lang`, `.disabled` marker) on
  the plugins partition; declares capabilities, linear memory, prerequisites; ECC slot 31 +
  R-Memory 501–511 pool.
- **OS Settings** — language, brightness, timezone, sleep interval, badge text, date/time,
  per-module enable state; NVS.
- **WiFi Configuration** — SSID, password, encryption, IP mode, intent flag; NVS (encrypted only
  inside a backup).
- **Encrypted Backup Container** — `backup.cdcbak` (AES-256-GCM, PBKDF2) on the plugins partition.
- **Attestation / Device Identity Key** — P-256 key in ECC slot 0, signs the PIN record, public-key
  hash mirrored in NVS for tamper detection; regenerated on wipe.

---

## External Interfaces & Integrations

| Boundary | Transport | Carries |
|----------|-----------|---------|
| Serial console | USB CDC (115200) and BLE (Nordic UART) | AUTH-gated management/diagnostics/secret-access command set; raw upload mode (CRC-32) |
| FIDO2 / WebAuthn | USB HID (CTAPHID, 64-byte reports) | CTAP2/U2F passkey registration + assertion |
| Keyboard auto-type | USB HID keyboard / BLE HID | Password/2FA fields typed as keystrokes |
| OTP challenge-response | USB HID feature reports (vendor, VID/PID `0x1D50:60FC`) | Yubico slot-2 HMAC-SHA1 challenge/response |
| OpenPGP smartcard | USB CCID (T=1, VID/PID `0x08E6:4433`) | OpenPGP card APDUs: sign, decipher, SSH auth, keygen |
| Badge-to-badge messaging | BLE GATT (cdc_msg service) | MIME-typed chunked payloads with ephemeral pairing |
| GPG cross-signing | BLE GATT (dedicated service) | Peer public-key transfer (encryption-required characteristics) |
| Companion tools | USB/BLE serial | `flash_firmware.py`, `upload.py`, `ble_serial.py`, `backup.py`, `coredump.py`, web flasher, plugin web installer |

Module enablement (factory defaults, `module_defaults.h`): enabled — mod_2fa, mod_fido2,
mod_password, mod_gpg, mod_sao, mod_vcard, mod_ble_serial, mod_nvsedit, mod_blehid, mod_vfat;
disabled — mod_usbhid, mod_otphid (the keyboard/OTP HID modules are mutually exclusive with each
other and with GPG CCID over the single keyboard HID slot).

[NEEDS CLARIFICATION: BLE HID descriptor/report details are referenced but not fully documented
in-repo.] [NEEDS CLARIFICATION: exact USB keyboard auto-type report format and inter-keystroke
throttling, and the role of the serial `PASTE` command in that flow.]

---

## Data Flows

- **WebAuthn registration**: host → CTAPHID makeCredential → on-device presence confirm → ECC
  key generated in TROPIC01 → credential + metadata + counter stored in R-Memory → packed
  self-signed attestation returned to host.
- **WebAuthn assertion**: host → getAssertion → presence (and PIN/UV if required) → per-credential
  counter incremented → challenge signed in the secure element → authData + signature returned.
- **Password auto-type**: holder selects entry → field emitted as HID keystrokes to the connected
  host. [NEEDS CLARIFICATION: precise flow/format as above.]
- **vCard send**: "Send vCard" → interactive peer pick → OFFER over Control → receiver consent →
  numeric-comparison pairing + link encryption → chunked Data + CRC32 Complete → receiver
  reassembles in PSRAM, validates, stores, emits exchange-complete event.
- **Backup export**: `BACKUP EXPORT <pass>` (serial) → modules serialise to JSON → AES-256-GCM with
  PBKDF2-derived key (random salt+nonce) → base64 container written to plugins partition.
- **Backup import**: read + base64-decode → parse header → derive key with stored iterations →
  AES-256-GCM decrypt → host-API-level check → per-record best-effort upsert → aggregate tally.
- **OTP challenge-response**: host writes 64-byte challenge as feature reports (CRC16-checked) →
  2FA responder computes 20-byte HMAC-SHA1 (optional touch) → response streamed back in chunks.
- **GPG cross-sign**: receive peer key over BLE → store in NVS → `GPG CROSS_SIGN <i>` builds an
  RFC 4880 certification preimage signed by the SIG ECC slot → `GPG EXPORT_SIGNED <i>` emits an
  armored block over serial.

---

## Authentication & Authorization

- **Badge PIN** authenticates the holder to the device UI and to the serial console (`AUTH`),
  sharing lockout state; 4–8 digits, retry budget 3 (only 1 granted at boot), 60-second
  self-recovering lockout (FR-001..003).
- **FIDO2 user presence** is a separate physical confirmation required for every FIDO2 operation,
  independent of badge unlock (FR-013). FIDO2 ClientPIN (CTAP2 v2) applies when a relying party
  requires user verification.
- **OpenPGP PW1/PW3** independently gate card operations vs. card administration; PW1 is
  admin-resettable, PW3 is terminal (FR-042).
- **Serial console** gates all commands except `PING`/`AUTH` behind the badge PIN (FR-081),
  including secret-bearing commands (TOTP/PASSWORD/GPG/BACKUP/TR01/NVS).
- **Plugin sandbox** authorises hardware/host access via manifest capabilities, enforced both at
  load time and per call (FR-072..074).
- **Badge-to-badge transfer** authorises each exchange via ephemeral numeric-comparison pairing,
  not persistent identity (FR-052).

[NEEDS CLARIFICATION: which operations require an *unlocked* device vs. only the relevant PIN —
e.g. backup export (FR-064), and whether the serial AUTH gate is on by default (FR-081).]

---

## Persistence & Data Storage

| Store | Location | Holds | Cleared by |
|-------|----------|-------|------------|
| NVS | partition `nvs` @ `0x9000`, `0x47000` (~287 KB) | OS settings, per-module state, WiFi config, module enable list (comma-separated, `mod_`-prefixed), attestation-key hash, boot/build-profile marker, cross-sign key store, plugin NVS (`plg_`/`plugin_` namespaces) | duress wipe; build-profile mismatch |
| TROPIC01 ECC slots (0–31) | secure element | private keys: attestation (0), GPG (1–3), CA (4), FIDO2 (5–30), plugin pool (31) | duress/factory wipe |
| TROPIC01 R-Memory (0–511, ~444 B/slot) | secure element | PIN record (0), GPG (1–3), CA (4), FIDO2 metadata (5–31), TOTP/HOTP/CR (32–131), password vault (132–500), plugin pool (501–511) | duress/factory wipe |
| FAT `plugins` | partition @ `0xDF0000`, `0x200000` (2 MB), mounted `/plugins` | `<id>.wasm`/`.aot`/`.meta`/`.lang`/`.disabled`, `i18n/lang_<code>.json`, `backup.cdcbak` | VFAT/PLUGIN delete; reformat |
| Coredump | partition @ `0xFF0000`, `0x10000` (64 KB) | crash dumps | overwrite |

Flash 16 MB; app partition `0x50000`, ~13.6 MB. 8 MB octal PSRAM @ 80 MHz is the default
allocation pool. The secure-element slot allocation is authoritative in `main/tropic_slot_map.h`.

[NEEDS CLARIFICATION: exact byte size per R-Memory slot (docs say ~444 B guaranteed, up to ~475
B runtime-dependent; defer to hardware/datasheet).] [NEEDS CLARIFICATION: orphaned-module NVS
cleanup trigger — what counts as a module "no longer existing" (compile-time removal vs runtime
disable).]

---

## Non-Functional Requirements

- **NFR-001 (Memory)**: Internal SRAM is the binding constraint; all non-interrupt, non-stack
  buffers MUST default to PSRAM. Main task stack is 24576 bytes.
- **NFR-002 (Flash wear / data durability)**: The device is pre-1.0; every firmware flash MAY
  wipe all on-device data, and there is NO migration code — breaking format changes wipe and
  reinit. Holders are expected to keep off-badge backups of critical material.
- **NFR-003 (Display)**: E-paper updates only on content change; refresh-mode discipline avoids
  unnecessary full refreshes (lock-screen clock stays PARTIAL_LIGHT).
- **NFR-004 (Crash safety)**: Security-critical state transitions (PIN retry counter in RAM,
  duress wipe boot marker reseeded only after wipe completes) MUST tolerate power loss without
  leaving a corrupt or half-wiped state.
- **NFR-005 (Sandbox isolation)**: Plugins MUST NOT be able to access blocked hardware or
  out-of-bounds memory; capability and pointer checks are enforced at the host boundary.
- **NFR-006 (Secrecy)**: Private keys generated on-chip MUST NOT leave the secure element in
  plaintext; the OpenPGP DEC key is the documented exception (decrypted to RAM per ECDH op,
  zeroized after use).
- **NFR-007 (Module isolation)**: Modules MUST be self-contained and added via a single
  registration point; core components MUST NOT reference modules; deleting a module MUST NOT
  break the build.
- **NFR-008 (Build profiles)**: `DEBUG_MODE` defaults ON (verbose logging including sensitive-
  value dumps) and MUST be disabled for production; toggling `DEBUG_MODE` or `FEATURE_SECURE_SERIAL`
  changes the build-profile byte and forces a factory wipe on next boot. The required release build
  profile (firmware version ≥ 1.0) is gated by SC-013; pre-1.0 builds are beta and exempt.

---

## Success Criteria *(mandatory)*

Measurable, technology-agnostic outcomes that verify the system behaves as specified.

- **SC-001**: A holder with the correct PIN reaches the main menu in a single unlock attempt
  100% of the time.
- **SC-002**: After the badge-PIN attempt budget is exhausted (one attempt immediately after a cold
  boot, up to three within a recovery window), the device refuses entry for 60 seconds and then
  accepts the correct PIN, with no firmware reflash ever required to recover (0% permanent brick
  rate for badge-PIN lockout).
- **SC-003**: A WebAuthn registration and a subsequent sign-in each complete with a single
  on-device confirmation within the displayed prompt window, and the signature counter increases
  on every successful sign-in.
- **SC-004**: A newly added TOTP account displays a code that a standard authenticator app
  accepts for the same secret (interoperable codes).
- **SC-005**: A stored password is delivered to a connected host exactly as entered when
  auto-type is triggered.
- **SC-006**: The badge is recognised as an OpenPGP smartcard by a standard host GPG toolchain
  without custom drivers.
- **SC-007**: An encrypted backup exported with a passphrase can be imported only with that same
  passphrase; a wrong passphrase or altered file is rejected 100% of the time, and import reports
  an accurate per-section success/failure tally.
- **SC-008**: No backup file ever contains secure-element private keys (FIDO2, GPG/SSH).
- **SC-009**: A duress-PIN entry leaves no on-device trace of the wipe intent before reboot, and
  after the next boot no previously stored secret is recoverable on-device.
- **SC-010**: A plugin that requests a blocked pin, an undeclared capability, or out-of-bounds
  memory is denied 100% of the time without crashing the device.
- **SC-011**: A vCard sent between two badges arrives byte-identical only after both holders
  confirm the same six-digit code.
- **SC-012**: The device runs from battery and reflects battery level and charge state on the
  lock screen, entering light sleep after the idle interval and deep sleep on the back-key hold.
- **SC-013**: A **release** build (firmware version ≥ 1.0) has `DEBUG_MODE=0`,
  `FEATURE_SECURE_SERIAL=1`, `FEATURE_PLUGIN_AOT=0` and `FEATURE_NVS_EDIT=0`; a release build
  violating any of these is rejected by the release gate 100% of the time. Pre-1.0 builds are
  **beta** and are not bound by this gate (`DEBUG_MODE` may remain on during beta).

---

## Assumptions

- The in-repo documentation under `website/src/content/docs/` plus the cited source headers are
  treated as the authoritative description of current behaviour; where docs and source disagree,
  the disagreement is recorded as a clarification item rather than resolved by guessing.
- "Current system" means the state of the working tree on branch `message-transfer-framework`,
  which includes the cdc_msg message-transfer framework and the website documentation migration.
- Hardware is CDC Badge v1.0/v1.1 (ESP32-S3, 16 MB flash, 8 MB octal PSRAM, TROPIC01,
  GDEY029T94 e-paper, BQ25895 charger, TCA9535 IO expander).
- The firmware version and host-API level are taken from `platformio.ini` / plugin docs and are
  user-managed; this spec does not assert a target version.
- Of the features previously flagged work-in-progress, the BLE vCard exchange, the message-transfer
  framework end-to-end, and BLE HID auto-type were **hardware-verified (2026-06-18)**. Still
  **Provisional (WIP, not hardware-verified)**: the BLE serial console, the GPG cross-sign send path,
  and the GPG/CCID UI (hardware acceptance non-blocking until verified; see Clarifications 2026-06-14).

---

## Documentation Discrepancies & Open Questions

The documentation in `website/src/content/docs/` is the intended long-term source of truth but
was newly generated; the items below are what must be reconciled before it can lead. **Category A**
are confirmed code-vs-doc mismatches (the doc is wrong and must be updated to match the code).
**Category B** are points the docs leave undocumented or that are unverified on hardware (resolve
by reading the code or testing the device, then document).

### Category A — confirmed documentation defects (doc must be fixed to match code)

- **D1 (Serial AUTH default)** — `serial-console.md` says `FEATURE_SECURE_SERIAL` is "enabled by
  default"; `feature_flags.h` defaults it to **0/off** (on only if Kconfig `CONFIG_SECURE_SERIAL`
  is set). The doc must state the real default. (FR-081)
- **D2 (PIN hashing is protocol-driven — NOT an inconsistency)** — *resolved as by-design.*
  Badge/FIDO2 PIN = `LEFT(SHA-256(PIN),16)` (forced by CTAP2 ClientPIN: the host sends exactly this
  value at verify time); OpenPGP PW1/PW3 = Iterated+Salted S2K (forced by the OpenPGP card KDF-DO);
  duress = S2K (internal, free choice). Unifying the KDFs would break FIDO2 or the OpenPGP card.
  **Doc action**: the security docs must *explain this rationale*, not present two KDFs as an
  inconsistency. (FR-007, FR-017)
- **D3 (DEBUG_MODE wording)** — `feature_flags.h` comments DEBUG_MODE as "disables lockouts", but
  the security docs state the **badge-PIN lockout recovery has no debug bypass**. Clarify and align:
  what does DEBUG_MODE actually disable vs. not. (FR-003, NFR-008)
- **D4 (Stale doc path in code)** — `feature_flags.h` still references the deleted `docs/SECURITY.md`;
  update the in-code reference to the `website/` security docs. (minor)

### Category B — undocumented / unverified (read code or test hardware, then document)

- **B1** *(RESOLVED)* FIDO2 ClientPIN and badge PIN are the **same secret** — shared
  `LEFT(SHA-256(PIN),16)` hash via `pin_storage_{get,verify}_fido2_hash`. (FR-017)
- **B2** `credProtect` levels parsed/stored but **not enforced** at assertion — intended end state or defect? (FR-016)
- **B3** Password-vault at-rest encryption — cipher/key/scope. (FR-032)
- **B4** Slot-0 attestation signature — algorithm/format/hash. (FR-006)
- **B5** Backup export — does it require an unlocked device beyond the passphrase? (FR-064)
- **B6** R-Memory slot byte size — exact bytes per slot. (Persistence)
- **B7** Orphaned-module NVS cleanup — trigger for "module no longer exists". (Persistence)
- **B8** WiFi scan/connect and NTP sync — exact timeout values. (FR-091)
- **B9** AUTH session idle timeout — exact value. (FR-081)
- **B10** BLE bond limit — behaviour when adding a 6th bonded device / eviction policy. (FR-090)
- **B11** USB keyboard auto-type — report format, throttling, role of serial `PASTE`. (Interfaces/Flows)
- **B12** BLE HID descriptor — report types / flow control. (Interfaces)
- **B13** GPG cross-sign — curve always labeled EdDSA from the status snapshot; P-256 may be
  mislabeled; unverified on hardware. (FR-044)
- **B14** Hardware status — cdc_msg framework, BLE vCard exchange and BLE HID auto-type were
  hardware-verified 2026-06-18; the BLE serial console, the GPG cross-sign send path and the
  GPG/CCID UI remain unverified on hardware. (FR-054, FR-044)
- **B15** Implementation details documented only at a high level — EventBus queue-overflow policy,
  ViewStack modal-depth semantics, plugin foreground→background demotion timing, module menu
  priority/sort order, plugin file-sandbox enforcement point, plugin file overwrite atomicity.
