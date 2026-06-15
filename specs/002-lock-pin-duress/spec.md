# Feature Specification: Lock / PIN / Lockout / Duress

**Feature Branch**: `002-lock-pin-duress`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the device
lock state, the badge PIN, brute-force lockout, the optional duress PIN, and PIN hashing.

> **Source of truth**: This spec lifts requirements FR-001..007 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are `cdc_core` `PinManager` and the
> `cdc_os_ui` lock screen. The self-destruct mechanics behind the duress trigger (FR-070) are owned
> by spec `013-persistence-factory-reset` and only referenced here.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Unlock the device with the badge PIN (Priority: P1)

The badge holder protects every on-device secret behind a numeric PIN. The device boots to a lock
screen; the holder must enter the correct PIN to reach any feature.

**Why this priority**: PIN unlock is the gate to every other capability; without it no other flow
is reachable.

**Independent Test**: Boot a provisioned badge, confirm it shows the lock screen, enter the correct
PIN followed by confirm, and confirm the main menu opens.

**Acceptance Scenarios**:

1. **Given** a provisioned badge at the lock screen, **When** the holder presses a key (other than
   the menu key) and enters the correct PIN followed by confirm, **Then** the main menu opens.
2. **Given** the lock screen, **When** the holder enters a PIN shorter than 4 digits or longer than
   8 digits, **Then** the device does not accept it as a valid unlock attempt.

---

### User Story 2 - Resist brute-force without bricking (Priority: P1)

Repeated wrong PIN entry is rate-limited so an attacker cannot guess freely, yet the legitimate
holder can never be permanently locked out by mistyping.

**Why this priority**: A security key that bricks itself on wrong entries is unusable; one that
allows unlimited guesses is insecure. The self-recovering lockout balances both.

**Independent Test**: Enter a wrong PIN until the attempt budget is exhausted (the device grants one
attempt immediately after a cold boot) and confirm the 60-second lockout with a visible countdown;
after it expires, confirm the correct PIN is accepted again without any reflash.

**Acceptance Scenarios**:

1. **Given** the lock screen, **When** the holder enters a wrong PIN and the attempt budget is
   exhausted (one attempt immediately after a cold boot; up to three within a recovery window),
   **Then** the device enters a 60-second lockout with a visible countdown and refuses further
   attempts until it expires.
2. **Given** an expired lockout, **When** the holder enters the correct PIN, **Then** the main menu
   opens and the retry budget is restored — no firmware reflash is ever required to recover.
3. **Given** a debug build and a release build, **When** the lockout is triggered in each, **Then**
   the recovery behaviour is identical (no debug bypass of badge-PIN lockout recovery).

---

### User Story 3 - Trigger a covert self-destruct with the duress PIN (Priority: P2)

The holder can configure an optional, default-off duress PIN. Entering it under coercion looks
exactly like a failed unlock while initiating a full wipe of all on-chip secrets.

**Why this priority**: Plausible-deniability self-destruct is a security feature for coercion
scenarios; it is optional and must never be distinguishable from a normal failed attempt.

**Independent Test**: Configure a duress PIN, enter it at the lock screen, observe that the UI,
logs and timing match a normal failed attempt, then confirm the wipe runs on the next boot.

**Acceptance Scenarios**:

1. **Given** a configured duress PIN, **When** that duress PIN is entered at unlock, **Then** the
   device behaves indistinguishably from a normal failed attempt (no UI, log, or timing tell) while
   initiating a full self-destruct (see FR-070) on the next boot.
2. **Given** the holder is setting a duress PIN, **When** the chosen duress PIN equals the badge PIN
   (or vice versa), **Then** the device rejects the configuration (bidirectional enforcement).
3. **Given** no duress PIN is configured (default), **When** any PIN is entered, **Then** no
   self-destruct is ever triggered.

---

### User Story 4 - Auto-lock on inactivity (Priority: P3)

An unattended unlocked device returns to the lock screen so secrets are not left exposed.

**Why this priority**: Reduces the window in which an unlocked, unattended badge is exploitable.

**Independent Test**: Unlock the device, leave it idle in a menu for 5 minutes, and confirm it
returns to the lock screen.

**Acceptance Scenarios**:

1. **Given** an unlocked session in a menu, **When** 5 minutes of inactivity elapse, **Then** the
   device returns to the lock screen.
2. **Given** a loaded plugin holding a sleep/lock inhibitor, **When** 5 minutes of inactivity
   elapse, **Then** the device does not auto-lock while the inhibitor is held.

---

### Edge Cases

- **Wrong PIN under lockout**: further attempts are refused until the 60-second timer expires; the
  retry counter lives in RAM so a power cycle mid-verify cannot corrupt it.
- **Interrupted duress wipe**: if power is lost mid-wipe, the boot marker remains absent and the
  wipe re-runs on next boot; the device cannot be left half-wiped (mechanics owned by FR-070).
- **Tampered PIN record**: a failed attestation-signature check on the slot-0 PIN record causes a
  silent reinit to defaults rather than a usable bypass.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The device MUST boot to a lock screen and require a numeric badge PIN (4-8 digits)
  before exposing any menu or feature.
- **FR-002**: The badge PIN retry budget is 3 attempts, tracked in RAM. On boot the device grants 1
  attempt (0 if it was locked at power-off) and starts a 60-second recovery timer; exhausting the
  budget on a wrong entry locks the device and (re)starts the 60-second timer with a visible
  countdown; on timer expiry the budget is restored to 3 and the locked flag is cleared.
- **FR-003**: The badge PIN MUST be self-recovering: the device MUST NOT be permanently bricked by
  repeated wrong badge-PIN entry, and the lockout recovery MUST behave identically in debug and
  release builds (no bypass).
- **FR-004**: The device MUST support an optional, default-off duress PIN that, when entered,
  initiates a full self-destruct (see FR-070, owned by spec 013) while being indistinguishable from
  a failed unlock (no UI, log, or timing tell); the duress PIN MUST differ from the badge PIN
  (bidirectionally enforced).
- **FR-005**: The device MUST auto-lock after 5 minutes of inactivity in menus (unless a loaded
  plugin holds a sleep/lock inhibitor).
- **FR-006**: The PIN record MUST be stored in TROPIC01 R-Memory slot 0 and bound to the device via
  an attestation signature; a failed signature check MUST cause a silent reinit to defaults.
  [NEEDS CLARIFICATION: exact attestation signature algorithm/format and hash for the slot-0 record
  - docs say "signed by the slot-0 attestation key" without specifying ECDSA/EdDSA. (baseline B4)]
- **FR-007**: PIN hashing is **protocol-determined and intentionally not uniform**: the badge PIN
  MUST be stored as `LEFT(SHA-256(PIN), 16)` because it doubles as the FIDO2 CTAP2 ClientPIN hash
  (the platform transmits exactly this 16-byte value at verification and the authenticator never
  sees the plaintext, so it cannot recompute an S2K hash); the OpenPGP PW1/PW3 PINs use Iterated+
  Salted S2K over SHA-256 (owned by spec 006); the duress PIN (internal only) also uses S2K.
  *(Code-confirmed: `PinManager::computeBadgeHash` vs `computeKdfHash`.)*

### Key Entities *(include if feature involves data)*

- **Badge PIN** — numeric unlock secret (4-8 digits, default `123456`), retry counter held in RAM,
  plus a persisted locked flag; stored as a PIN record in TROPIC01 R-Memory slot 0,
  attestation-signed. Hash is `LEFT(SHA-256(PIN), 16)` (shared with FIDO2 ClientPIN).
- **Duress PIN** — optional alternate PIN (default off), stored with an S2K hash; entry triggers a
  full self-destruct (FR-070) indistinguishably from a failed unlock. Must differ from the badge PIN.
- **Lockout state** — RAM-resident retry counter (budget 3; only 1 granted immediately after boot,
  0 if locked) and a 60-second recovery timer with a visible countdown; shared with the serial
  console AUTH gate (owned by spec 012).
- **Attestation / Device Identity Key** — P-256 key in ECC slot 0 that signs the PIN record; a
  failed signature check on slot-0 reinits the PIN record to defaults (key lifecycle owned by
  spec 013).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A holder with the correct PIN reaches the main menu in a single unlock attempt 100% of
  the time.
- **SC-002**: After the badge-PIN attempt budget is exhausted (one attempt immediately after a cold
  boot, up to three within a recovery window), the device refuses entry for 60 seconds and then
  accepts the correct PIN, with no firmware reflash ever required to recover (0% permanent brick
  rate for badge-PIN lockout).
- **SC-003**: A duress-PIN entry leaves no on-device trace of the wipe intent before reboot (UI,
  log, and timing are indistinguishable from a failed attempt), and after the next boot no
  previously stored secret is recoverable on-device.
- **SC-004**: An unattended unlocked device returns to the lock screen within the inactivity
  interval 100% of the time unless a sleep/lock inhibitor is held.

## Assumptions

- The self-destruct/factory-wipe mechanics referenced by FR-004 (FR-070) are owned and verified by
  spec `013-persistence-factory-reset`; this spec only specifies the duress *trigger* behaviour.
- Hardware-backed acceptance (lockout recovery, duress wipe) is verified on a CDC Badge v1.0/v1.1
  with a TROPIC01 secure element; the host test tier cannot exercise secure-element-bound state.
- The default badge PIN is `123456`; provisioned devices are expected to change it.
- The badge PIN hash format (`LEFT(SHA-256(PIN), 16)`) is fixed by the FIDO2 CTAP2 ClientPIN
  protocol and cannot be unified with the OpenPGP S2K hash without breaking one of them.
