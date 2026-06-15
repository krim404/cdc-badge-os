# T-HIL03 — BLE badge-to-badge vCard transfer (numeric comparison) + abuse budgets

**Status: non-blocking (hardware verification)**

**Provisional / WIP** — the cdc_msg message-transfer framework and BLE vCard exchange are
documented as work-in-progress and **not yet verified on hardware** (spec FR-054, open items
B14; Clarifications 2026-06-14). This plan defines the intended acceptance; passing it is what
graduates the feature out of WIP.

**FRs covered**: FR-050, FR-051, FR-052, FR-053, FR-054

Verifies a generic MIME-typed badge-to-badge transfer over BLE GATT with chunked framing and
CRC32 completion, routed to the `text/vcard` handler, gated by ephemeral numeric-comparison
pairing confirmed on both badges, with the framework's size limits and abuse-resistance budgets.
Maps to Success Criterion SC-011.

## Prerequisites

### Hardware
- **Two** CDC Badge v1.0/v1.1 units (sender + receiver), both with working BLE.
- Both unlocked; the receiver with the message-transfer **beacon enabled** (Tools → beacon).

### Host tools
- None required for the core round-trip (badge-to-badge over BLE).
- Optional: serial terminal at 115200 baud on each badge for log/diagnostic inspection.

### Build profile
- `mod_vcard` and `cdc_msg` present and enabled on both badges (default-enabled set).
- Both badges have an own vCard set (sender) so there is something to send.

## Procedure

1. **Beacon up (FR-050/FR-052)**: on the receiver, enable the transfer beacon under Tools.
   On the sender, edit/confirm the own vCard.
2. **Offer + peer pick (FR-054)**: on the sender, choose "Send vCard", scan, and pick the
   receiver from the peer list.
3. **Numeric comparison (FR-052)**: both badges display a six-digit code. Confirm the codes
   **match**, then press confirm on **both** badges.
4. **Transfer + dedup (FR-050/FR-051/FR-054)**: allow the chunked transfer to complete. On the
   receiver, confirm the vCard was stored and the exchange-complete result is shown.
5. **Round-trip integrity (SC-011)**: open the received vCard on the receiver and compare its
   text byte-for-byte with the sender's own vCard (compare on-screen fields, or dump both over
   serial and diff).
6. **Exact-text dedup (FR-054)**: repeat steps 2–4 sending the **same** vCard again. Confirm the
   receiver does not create a duplicate (dedup by exact text).
7. **Mismatch rejection (FR-052)**: repeat steps 2–3 but on one badge confirm a code that does
   NOT match (or decline). Confirm the transfer does not proceed and nothing is stored.
8. **No-handler decline (FR-051)**: if a tool to send an unregistered MIME type is available
   (e.g. a plugin using `host_msg_*`), send a payload with a MIME type no badge handles. Confirm
   the receiver declines. (Skip if no such sender is available; record as not-exercised.)
9. **Size limits (FR-053)**: attempt to send an oversized payload (> 4096 bytes), an oversized
   MIME string (> 63 bytes), or an oversized peer name (> 31 bytes) using the plugin
   `host_msg_*` path or a serial harness. Confirm each is declined as too large / malformed.
10. **Abuse budgets (FR-053 / spec Edge Cases)**: from the sender, fire offers rapidly at the
    receiver.
    - Confirm the receiver rate-limits inbound prompts: global budget ~5 prompts / 30 s and
      per-connection ~3 / 10 s.
    - Decline an offer and immediately re-offer; confirm a post-decline cooldown blocks the
      immediate re-prompt.
    - Queue more than 4 offers; confirm the receiver replies Busy once the offer queue (≤ 4) is
      full.
    - The bond MUST be forgotten after each transfer (ephemeral pairing) — confirm a subsequent
      transfer requires a fresh numeric-comparison confirmation.

## Pass criteria

- Step 3–5: the vCard transfers and the received text is **byte-identical** to the sender's,
  only after both holders confirmed the same six-digit code (SC-011).
- Step 6: re-sending identical text creates no duplicate on the receiver.
- Step 7: a mismatched/declined numeric comparison aborts the transfer; nothing is stored.
- Step 8 (if exercised): an unhandled MIME type is declined by the receiver.
- Step 9: payload > 4096 B, MIME > 63 B, or name > 31 B is rejected as too large/malformed.
- Step 10: inbound prompts are rate-limited (≈5/30 s global, ≈3/10 s per connection), a
  post-decline cooldown suppresses immediate re-prompts, a full offer queue (> 4) replies Busy,
  and each transfer requires fresh pairing (bond forgotten after transfer).

## Notes

- **WIP status**: failures here do NOT block the build (non-blocking). Record outcomes against
  B14; keep FR-044/FR-050..054 flagged WIP in their per-capability specs until this plan passes
  on hardware.
- **Flash conservation**: no flashing required. Flash both badges from one image and run the
  full plan. If a reflash is needed, use the serial `AUTH <pin>` then `BOOTLOADER` path.
- **Budget timing**: the rate-limit windows are short (10 s / 30 s); pace offers with a stopwatch
  and record the exact count and timing observed so the ~5/~3 budgets can be confirmed.
- Two physical badges are mandatory; this round-trip cannot be exercised host-side.
