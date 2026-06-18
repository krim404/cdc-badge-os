---
title: cdc_msg message-transfer protocol
description: The generic MIME-typed badge-to-badge BLE transfer framework — GATT profile, framing, ephemeral numeric-comparison pairing, chunking and handler registration.
sidebar:
  order: 12
---

`cdc_msg` (`components/cdc_msg/`) is the generic badge-to-badge transfer
framework. It sends a typed payload (a MIME type plus raw bytes) to a nearby
badge over a lightweight GATT profile, asks the receiving user for consent,
encrypts the link with an ephemeral pairing, and delivers the bytes to whatever
module or plugin registered a handler for that MIME type. The core
(`MessageTransfer`) is headless; the OS UI layer renders the prompts.

## Roles

- **Sender (central)** connects out to a peer, discovers the service, offers a
  typed payload, and after consent streams the bytes.
- **Receiver (peripheral)** advertises the service via the *beacon*, accepts an
  offer after user consent, and reassembles the payload.

Both roles run from one `MessageTransfer` singleton, driven by `tick()` on the
main task. NimBLE host-task callbacks only set flags / copy small data under a
short mutex try-lock; `tick()` advances both state machines and does the heavy
work (delivery, NVS, teardown).

## GATT profile

A 128-bit random base UUID is used, with the discriminator at byte index 12. The
layout mirrors the Nordic UART Service pattern.

| Role | UUID | Properties | Direction |
| --- | --- | --- | --- |
| Service | `CDC50001-B0A7-4E3D-9C82-5A6F1B3E9D2C` | — | advertised so scanners find a badge |
| Control | `CDC50002-...` | WRITE (plaintext) | sender -> receiver |
| Status | `CDC50003-...` | NOTIFY (plaintext) | receiver -> sender |
| Data | `CDC50004-...` | WRITE / WRITE_NO_RSP (**encrypted**) | sender -> receiver |

The Data characteristic permission is `WRITE_ENC`, so chunks can only be written
after the link is encrypted. Control and Status are plaintext: the offer and the
accept/decline/progress/error signalling happen before encryption.

The receiver advertises the service UUID plus its Complete Local Name; there is
no proprietary manufacturer data. Discovery filters scan results to advertisers
carrying the service UUID.

## Framing

All multi-byte fields are little-endian. Every field is bounds-checked against
fixed limits before use; wire data is never trusted. The wire protocol version
is `1`; a mismatch is rejected.

### Control (sender -> receiver), byte 0 = opcode

| Opcode | Value | Layout |
| --- | --- | --- |
| `Offer` | `0x01` | `[op][ver][u32 totalLen][mimeLen][nameLen][mime...][name...]` (8-byte fixed header) |
| `Abort` | `0x02` | sender aborts the in-flight transfer |

The `nameLen` byte carries the name length in its low 5 bits (`0..31`); bit 7 is
a flag that requests a [session-persistent pairing](#pairing). The receiver
validates the OFFER bounds-first: header length, version, a non-zero
`mimeLen <= 63`, the masked `nameLen <= 31`, that `8 + mimeLen + nameLen` fits
the frame, and that `0 < totalLen <= 4096`. A bad frame is declined with
`BadFrame`; an over-size payload with `TooLarge`.

### Data (sender -> receiver), byte 0 = opcode

| Opcode | Value | Layout |
| --- | --- | --- |
| `Chunk` | `0x10` | `[op][reserved][bytes...]` (2-byte header) |
| `Complete` | `0x11` | `[op][reserved][u32 crc32]` (6 bytes) |

Chunk bounds use a subtraction form that cannot wrap: a chunk is rejected if it
would push `received` past `totalLen`. `Complete` is rejected unless exactly
`totalLen` bytes were received; the CRC is verified in `tick()` before delivery.

### Status (receiver -> sender), byte 0 = opcode

| Opcode | Value | Meaning |
| --- | --- | --- |
| `Accept` | `0x01` | receiver accepted; sender may start encryption |
| `Decline` | `0x02` | `[op][Reason]` receiver declined |
| `Progress` | `0x03` | `[op][u32 received]` receive progress |
| `Done` | `0x04` | payload received and delivered |
| `Error` | `0x05` | `[op][Reason]` transfer aborted |
| `Busy` | `0x06` | receiver saturated; try again later |
| `Queued` | `0x07` | offer queued behind an active transfer |

Progress notifications are throttled to at most every 512 received bytes (and
always on the final byte).

### Reason codes

`None(0)`, `NoHandler(1)`, `UserDeclined(2)`, `TooLarge(3)`, `Busy(4)`,
`Timeout(5)`, `BadFrame(6)`, `CrcMismatch(7)`, `Disconnected(8)`,
`PairFailed(9)`.

## Transfer sequence

1. **Connect & discover.** The sender connects to the peer and discovers the
   service, locating the Control, Status and Data value handles. (The Status
   CCCD is taken as `statusHandle + 1`.)
2. **Subscribe & offer.** The sender enables Status notifications, then writes an
   OFFER carrying the MIME type, the sender's beacon name and `totalLen`. The
   OFFER is bounded to both the negotiated MTU and the frame buffer; if it does
   not fit, the sender name is truncated, and if even the MIME does not fit the
   send fails with `BadFrame`.
3. **Handler check & rate limit.** The receiver declines with `NoHandler` if no
   local module/plugin (live or deferred) handles the MIME type, replies `Busy`
   on a per-connection rate-limit hit, otherwise enqueues the offer.
4. **Consent.** When the offer reaches the head of the queue and the prompt
   budget allows, the receiver moves to *AwaitingConsent* and publishes
   `BLE_CONSENT_REQUEST`; the UI renders the prompt. On accept the receiver
   notifies `Accept`; on decline it notifies `Decline(UserDeclined)` and starts
   a quiet cooldown. A persistent offer from a peer already trusted this session
   skips this step (and the prompt budget): the receiver notifies `Accept`
   straight away. See [Pairing](#pairing).
5. **Encrypt (ephemeral pairing).** On `Accept` the sender calls
   `initiateSecurity()`, triggering numeric-comparison pairing. When the link is
   encrypted, the receiver allocates the reassembly buffer (PSRAM) and moves to
   *Receiving*; the sender moves to *Streaming*.
6. **Stream.** The sender writes `Chunk` frames sized to the negotiated MTU
   (clamped to the stack frame buffer), then a `Complete` frame with the CRC32.
   The receiver notifies `Progress` as bytes arrive.
7. **Deliver.** On `Complete`, `tick()` verifies the CRC and the exact length,
   then dispatches to the registered handler (or the deferred plugin resolver).
   On success the receiver notifies `Done` and publishes
   `BLE_EXCHANGE_COMPLETE`; a short grace period lets the `Done` notification
   flush before the link is dropped.

State machines:

- Sender: `Idle -> PickingPeer -> Connecting -> Discovering -> Offering ->
  AwaitingEncrypt -> Streaming -> Done | Failed`.
- Receiver: `Idle -> AwaitingConsent -> AwaitingEncrypt -> Receiving ->
  Delivering -> Done | Failed`.

## Pairing

The link is encrypted with a **numeric-comparison** pairing, the same mechanism
host pairing uses. By default the bond is **ephemeral**: each side records the
peer's identity address while encrypting and forgets the bond (`forgetBond`)
when the connection drops, so a transfer leaves no persistent bond behind. The
bond-tracking table is sized to the controller's connection capacity; a full
table is logged as a warning since an un-forgotten bond is security-relevant.

### Session-persistent pairing

A send may opt in to a **session-persistent** pairing by setting bit 7 of the
OFFER `nameLen` byte (`kOfferFlagPersist`). When set, both sides add the peer's
identity address to an in-RAM trusted set once the link is encrypted and keep
the bond instead of forgetting it on disconnect. A later send to the same peer
then reconnects and re-encrypts from the stored link key with no
numeric-comparison prompt; on the receiver, an offer from a trusted peer is
auto-accepted without the consent prompt.

The trust is runtime-only and is **never persisted across a reboot**. Remembered
bonds are dropped at clean service teardown, and a boot-time janitor clears any
left in the shared bond store by a previous run (tracked via an address-only
ledger in the `msg` NVS namespace, cleared once acted upon). The trusted set is
capped at the controller's bond capacity.

## Limits

| Limit | Value |
| --- | --- |
| Max payload per transfer | 4096 bytes |
| MIME type length (excl. NUL) | 63 bytes |
| Peer name length (excl. NUL) | 31 bytes |
| Registered MIME handlers | 8 |
| Pending offer queue depth | 4 |

### Timeouts

| Phase | Timeout |
| --- | --- |
| Connect | 10 s |
| Discovery | 5 s |
| Consent | 30 s |
| Encryption (numeric comparison) | 30 s |
| Transfer idle | 8 s |

## Abuse resistance

- **Global prompt budget** (address-independent, the authoritative control,
  because BLE addresses rotate): at most 5 consent prompts per 30 s window.
- **Quiet cooldown**: a 20 s global quiet period after the user declines an
  offer.
- **Per-connection rate table** (best-effort, 8 entries): at most 3 offers per
  10 s window per connection.
- A full offer queue replies `Busy` (graceful saturation) rather than dropping
  silently.

## Registering a handler (firmware)

A module registers a delivery handler for a MIME type:

```cpp
cdc::msg::MessageTransfer::instance().registerHandler(
    "text/vcard",            // MIME type, 1..63 bytes
    "mod_vcard.received",    // i18n key (or literal) shown in the consent prompt
    deliverVcard);           // DeliverFn callback
```

The `DeliverFn` signature is:

```cpp
bool deliver(const uint8_t* data, uint32_t len,
             const char* mime, const char* peerName);
```

`data` is **untrusted, attacker-controlled and not NUL-terminated**; the handler
must validate it and copy into a bounded buffer before parsing. Return `true`
only if the payload was accepted/stored. The registry holds up to 8 handlers.
`unregisterHandler(mime)` removes one. See the
[vCard handler](/dev/proto/vcard/) for a concrete example.

### Sending

- `beginInteractiveSend(mime, data, len, persistent = false)` buffers the
  payload and requests the interactive peer picker (the UI confirms a target,
  then the framework connects).
- `sendTo(addr, addrType, mime, data, len, persistent = false)` sends directly
  to a known peer with no picker.

Both copy the payload into PSRAM and refuse if a send is already in progress or
arguments are invalid. `persistent = true` opts the transfer into a
[session-persistent pairing](#session-persistent-pairing).

## Deferred (plugin) handlers

Plugins may register handlers for MIME types even when the plugin is not loaded.
`setDeferredHandler(canHandle, deliver)` installs a resolver: when an OFFER
arrives for a MIME type no live handler covers, the framework asks `canHandle`
whether some installed plugin handles it (host-task safe), and on completion
hands the payload to `deferredDeliver`, which defers the actual plugin load and
dispatch to the plugin task. This resolver is installed once by the plugin
manager at init.

Plugins access the framework through the host API
(`host_msg_register_handler` / `host_msg_send_interactive` / `host_msg_send` /
`host_msg_consume`), with `HOST_MSG_PAYLOAD_MAX` (4096) and `HOST_MSG_MIME_MAX`
(64) matching the framework limits.

## Events

The framework publishes two `EventBus` events the UI subscribes to:

- `BLE_CONSENT_REQUEST` — an inbound offer reached *AwaitingConsent*; show the
  prompt.
- `BLE_EXCHANGE_COMPLETE` — a transfer finished (success or failure); read the
  result via `consumeResult()` and show a toast.
