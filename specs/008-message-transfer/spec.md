# Feature Specification: Badge-to-Badge Message Transfer

**Feature Branch**: `008-message-transfer`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the generic
MIME-typed badge-to-badge transfer framework (`cdc_msg`) over BLE GATT, its handler-routing model,
ephemeral numeric-comparison pairing, framing limits and abuse-resistance budgets, and the vCard
module's use of it.

> **Hardware-verified (2026-06-18)**: the cdc_msg badge-to-badge messaging framework end-to-end and
> BLE vCard exchange (FR-050..054) were verified on hardware (on-device test run; SC-005 / HIL plan
> T-HIL03). The statements below describe the as-built, hardware-verified behaviour.

> **Source of truth**: This spec lifts requirements FR-050..054 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are `cdc_msg` (the transfer
> framework) and `mod_vcard` (the first consumer). The single BLE-controller invariant (FR-090) is
> owned by spec `011-ble-controller-hid`; the plugin `host_msg_*` API surface is owned by spec
> `010-plugin-runtime-hostapi`.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Send a vCard to a nearby badge (Priority: P1)

The holder shares their own vCard with a nearby peer badge over BLE; both holders confirm a matching
six-digit code, the vCard transfers, and the receiver stores it.

**Why this priority**: vCard exchange is the first and primary consumer of the framework and the
end-to-end user-visible value; it exercises the whole transfer path.

**Independent Test (Provisional)**: With two badges and the beacon enabled on the receiver, send a
vCard from the sender, confirm the matching six-digit code on both, and confirm the receiver stores
the vCard deduplicated by exact text.

**Acceptance Scenarios**:

1. **Given** two badges with the beacon enabled on the receiver, **When** the sender selects "Send
   vCard", picks the peer, and both confirm the matching six-digit code, **Then** the vCard transfers
   over the cdc_msg `text/vcard` handler and the receiver stores it.
2. **Given** the receiver already holds an identical vCard, **When** the same vCard is received again,
   **Then** it is deduplicated by exact text (not stored twice).
3. **Given** an inbound `text/vcard` transfer for which a handler is registered, **When** the
   transfer completes, **Then** the CRC32 completion check passes before the payload is accepted.

---

### User Story 2 - Route inbound transfers by MIME type (Priority: P1)

The framework routes each inbound transfer to a handler registered for its MIME type, and declines
transfers for which no handler exists.

**Why this priority**: Handler routing is the core abstraction that makes the framework generic and
safe; without it inbound data has no consumer and could be mishandled.

**Independent Test (Provisional)**: Register a handler for one MIME type, offer a transfer of that
type (accepted) and one of an unregistered type (declined), and confirm the routing decisions.

**Acceptance Scenarios**:

1. **Given** a handler registered for a MIME type, **When** a transfer of that type is offered and
   confirmed, **Then** the framework routes the completed payload to that handler.
2. **Given** no handler registered for a MIME type, **When** a transfer of that type is offered,
   **Then** the framework declines the transfer.
3. **Given** the handler registry is full (≤ 8 handlers), **When** a further handler registration is
   attempted, **Then** the registration does not exceed the limit.

---

### User Story 3 - Confirm transfers with ephemeral numeric-comparison pairing (Priority: P2)

Each exchange is authorized by an ephemeral numeric-comparison pairing confirmed on both badges; the
bond is forgotten after the transfer so identity is per-exchange, not persistent.

**Why this priority**: Numeric-comparison confirmation is the trust gate that prevents silent or
spoofed transfers; the ephemeral bond keeps the security model per-exchange.

**Independent Test (Provisional)**: Initiate a transfer, confirm the six-digit code matches on both
badges, complete the transfer, and confirm the pairing/bond is not retained afterwards.

**Acceptance Scenarios**:

1. **Given** an offered transfer, **When** both badges display a six-digit code and both holders
   confirm it matches, **Then** the link is encrypted and the transfer proceeds.
2. **Given** the codes do not match (or one side declines), **When** the holder rejects, **Then** the
   transfer does not proceed.
3. **Given** a completed transfer, **When** the exchange ends, **Then** the ephemeral bond is
   forgotten (no persistent pairing is retained).

---

### User Story 4 - Resist transfer-prompt abuse (Priority: P3)

The framework limits how often inbound-transfer prompts can interrupt the holder, so a hostile peer
cannot spam confirmation dialogs or wedge the device.

**Why this priority**: Without rate limits an attacker in BLE range could flood the holder with
prompts (a denial-of-service and a social-engineering vector); the budgets bound the abuse.

**Independent Test (Provisional)**: Issue rapid repeated offers from a peer and confirm the global
and per-connection budgets throttle prompts, a declined offer triggers a cooldown, and a full offer
queue replies Busy.

**Acceptance Scenarios**:

1. **Given** rapid inbound offers, **When** they exceed the global budget (~5 prompts / 30 s) or the
   per-connection budget (~3 / 10 s), **Then** further prompts are throttled rather than shown.
2. **Given** the holder declines an offer, **When** the same peer re-offers immediately, **Then** a
   post-decline cooldown suppresses the new prompt.
3. **Given** the offer queue is full (≤ 4 queued offers), **When** a further offer arrives, **Then**
   the framework replies Busy.
4. **Given** a payload over 4096 bytes, an oversized MIME (> 63 bytes) or name (> 31 bytes) field, or
   a malformed frame, **When** it is offered, **Then** the transfer is declined (TooLarge / BadFrame).

---

### Edge Cases

- **Message-transfer abuse**: inbound transfer prompts are rate-limited (global ~5 prompts/30 s and
  per-connection ~3/10 s budgets, plus a post-decline cooldown); a full offer queue (≤ 4) replies Busy.
- **vCard / message too large**: payloads over 4096 bytes, MIME fields over 63 bytes, peer-name
  fields over 31 bytes, or malformed frames are declined.
- **No handler for MIME type**: the transfer is declined.
- **Code mismatch / decline**: a non-matching numeric-comparison code or a decline on either side
  aborts the transfer with no payload stored.
- **Duplicate vCard**: a received vCard identical (by exact text) to a stored one is not stored twice.
- **Hardware-verified (2026-06-18)**: the framework end-to-end and BLE vCard exchange were verified on hardware.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-050**: The device MUST provide a generic MIME-typed badge-to-badge
  transfer over BLE GATT (Control/Status/Data characteristics) with chunked framing and a CRC32
  completion check.
- **FR-051**: Inbound transfers MUST be routed to a registered handler by MIME
  type; with no handler the transfer MUST be declined.
- **FR-052**: Transfers MUST require ephemeral numeric-comparison pairing
  confirmed on both badges; the bond MUST be forgotten after the transfer.
- **FR-053**: The framework MUST enforce limits: payload ≤ 4096 bytes, MIME
  ≤ 63 bytes, peer name ≤ 31 bytes, ≤ 8 registered handlers, ≤ 4 queued offers; plus abuse-resistance
  budgets (global ~5 prompts/30 s, per-connection ~3/10 s, post-decline cooldown).
- **FR-054**: The vCard module MUST register a `text/vcard` handler and provide
  "Send vCard"; received vCards MUST be deduplicated by exact text.

### Key Entities *(include if feature involves data)*

- **Message Transfer** — an ephemeral MIME-typed payload (≤ 4096 bytes) carried over BLE with a
  sender identity and a six-digit numeric-comparison code; framed in chunks with a CRC32 completion
  check. MIME type ≤ 63 bytes, peer name ≤ 31 bytes.
- **Transfer Handler** — a registration mapping a MIME type to a consumer; at most 8 may be
  registered. The vCard module registers a `text/vcard` handler.
- **Offer Queue** — the bounded queue of pending inbound transfer offers (≤ 4); a full queue replies
  Busy.
- **Abuse Budget** — the rate-limit state: global (~5 prompts / 30 s), per-connection (~3 / 10 s),
  and a post-decline cooldown.
- **vCard / Contact** — a vCard 4.0 record (≤ 768 bytes): the holder's own card plus received cards
  (≤ 100), deduplicated by exact text. Owned by `mod_vcard`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: An inbound transfer with a registered MIME handler is routed to that handler; one with
  no handler is declined 100% of the time.
- **SC-002**: A payload over 4096 bytes, a MIME field over 63 bytes, a name field over 31 bytes, or a
  malformed frame is declined (TooLarge / BadFrame) 100% of the time.
- **SC-003**: The framework registers at most 8 handlers and queues at most 4 offers; a full queue
  replies Busy and never wedges the device.
- **SC-004**: Inbound-prompt rate limits hold: bursts exceeding the global (~5/30 s) or per-connection
  (~3/10 s) budgets are throttled, and a declined offer triggers a post-decline cooldown.
- **SC-005** *(hardware-verified 2026-06-18)*: On hardware, a vCard sent between two badges arrives
  byte-identical only after both holders confirm the same six-digit code, the receiver dedups by
  exact text, and the ephemeral bond is not retained. Verified by HIL plan T-HIL03.

## Assumptions

- This capability is **hardware-verified (2026-06-18)** via the on-device test run (HIL plan T-HIL03).
- The BLE transport runs through the single shared controller (`IBluetoothController`); the
  single-controller invariant and bonding model are owned by spec `011-ble-controller-hid`.
- The framework is consumed by `mod_vcard` for `text/vcard` and is exposed to plugins via the
  `host_msg_*` host API (owned by spec `010-plugin-runtime-hostapi`).
- The numeric values for limits and budgets (payload ≤ 4096, MIME ≤ 63, name ≤ 31, ≤ 8 handlers,
  ≤ 4 queued, global ~5/30 s, per-connection ~3/10 s) are taken from the baseline; the "~" budgets
  are approximate and the exact constants defer to the code.
- Host-tier tests can cover message-transfer framing and bounds (mime ≤ 63, name ≤ 31, total ≤ 4096,
  opcodes, BadFrame/TooLarge — T-H07) and vCard 4.0 mapping/parse with exact-text dedup (T-H05), but
  not the BLE round trip or numeric-comparison pairing.
