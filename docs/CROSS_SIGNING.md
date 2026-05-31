# GPG Cross-Signing Protocol (Badge2Badge)

Protocol specification for exchanging and cross-signing GPG public keys between CDC Badges via BLE.

> **Related:** [GPG Module](GPG.md) | [BLE vCard Protocol](ble_vcard_protocol.md)

## Overview

Cross-signing enables:
- Exchange of GPG public keys between badges
- Signing received keys (Web of Trust)
- Personal verification at conferences/meetups

## Concept

```
Badge A                             Badge B
   │                                   │
   │  ─────── Send GPG Key ─────────►  │
   │                                   │
   │  ◄────── Receive GPG Key ───────  │
   │                                   │
   │      ┌─────────────────────┐      │
   │      │  In-Person Check:   │      │
   │      │  Compare            │      │
   │      │  Fingerprints       │      │
   │      └─────────────────────┘      │
   │                                   │
   │  ─────── Cross-Signature ───────► │
   │                                   │
   │  ◄────── Cross-Signature ──────── │
   │                                   │
```

## BLE Protocol

### Service and Characteristics

Dedicated GATT service, separate from the vCard exchange service:
- Service: `8E2F1F30-8B5D-4D7A-9A6E-4C9D6A8B1A01`
- **RX** (Write): `8E2F1F31-8B5D-4D7A-9A6E-4C9D6A8B1A01` - peer pushes its public key here
- **Status** (Read + Notify): `8E2F1F32-8B5D-4D7A-9A6E-4C9D6A8B1A01` - server reports completion

### Opcodes

Transport mirrors the vCard exchange ([BLE vCard Protocol](ble_vcard_protocol.md)):

| Opcode | Name | Direction | Description |
|--------|------|-----------|-------------|
| `0x11` | GPG_WRITE_START | Client→Server | Start GPG key transfer, +2 byte length |
| `0x12` | GPG_WRITE_CONT | Client→Server | Continue |
| `0x13` | GPG_WRITE_END | Client→Server | Complete |
| `0x91` | GPG_DATA_START | Server→Client | Start GPG key response, +2 byte length |
| `0x92` | GPG_DATA_CONT | Server→Client | Continue |
| `0x93` | GPG_DATA_END | Server→Client | Complete; trailing 1-byte status code |

The `0x93` END frame carries a trailing status byte:

| Status | Meaning |
|--------|---------|
| `0x00` | OK |
| `0x01` | Bad payload |
| `0x02` | Store full |
| `0x03` | Internal error |

### Payload Format

```
┌─────────┬────────────┬─────────────┬─────────────┬─────────────┬────────────┐
│ Curve   │ PubKey Len │ Public Key  │ Fingerprint │ UserID Len  │ User ID    │
│ 1 Byte  │ 1 Byte     │ 32/64 Bytes │ 20 Bytes    │ 1 Byte      │ max 63 B   │
└─────────┴────────────┴─────────────┴─────────────┴─────────────┴────────────┘
```

| Field | Size | Description |
|-------|------|-------------|
| Curve | 1 | `1` = Ed25519, `2` = P-256 |
| PubKey Len | 1 | 32 for Ed25519, 64 for P-256 |
| Public Key | 32/64 | Raw public key bytes |
| Fingerprint | 20 | SHA-1 GPG fingerprint |
| UserID Len | 1 | Length of User ID |
| User ID | max 63 | "Name <email>" (UTF-8) |

**Maximum Payload:** 150 Bytes

### Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Exchange Flow                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  Client (Initiator)              Server (Responder)                  │
│        │                                │                            │
│        │──── Connect + Pairing ────────►│                            │
│        │                                │                            │
│        │──── GPG_WRITE_START (len) ────►│                            │
│        │──── GPG_WRITE_CONT ───────────►│                            │
│        │──── GPG_WRITE_END ────────────►│                            │
│        │                                │                            │
│        │◄─── GPG_DATA_START (len) ──────│                            │
│        │◄─── GPG_DATA_CONT ─────────────│                            │
│        │◄─── GPG_DATA_END ──────────────│                            │
│        │                                │                            │
│        │──── Disconnect ───────────────►│                            │
│        │                                │                            │
└─────────────────────────────────────────────────────────────────────┘
```

## Cross-Signature

### Data Being Signed

The cross-signature is a full RFC 4880 V4 certification signature (type `0x10`)
over the target's public-key packet and User ID packet, including the hashed
subpackets and a signature creation timestamp. It is produced with the badge's
own GPG SIG key (the SIG ECC slot returned by `gpg_storage_sig_slot()`).

### Signature Format

| Curve | Signature Length | Format |
|-------|------------------|--------|
| Ed25519 | 64 Bytes | R (32) + S (32) |
| P-256 | 64 Bytes | R (32) + S (32) |

## Storage

### NVS Schema

- **Namespace:** `gpg_recv`
- **Key Format:** `pk_<fingerprint_hex_8>` (first 4 bytes of V4 FP, hex)

### Structure

```c
typedef struct {
    uint8_t curve;                  // 1 Byte (CDC_CURVE_ED25519 / CDC_CURVE_P256)
    char user_id[64];               // 64 Bytes, null-padded UTF-8
    uint8_t pubkey[64];             // 64 Bytes (32 used for Ed25519, 64 for P-256)
    uint8_t pubkey_len;             // 1 Byte
    uint8_t fingerprint_v4[20];     // 20 Bytes (RFC 4880, SHA-1)
    uint8_t fingerprint_v5[32];     // 32 Bytes (RFC 9580, SHA-256)
    uint32_t received_at;           // 4 Bytes (Unix timestamp)
    uint8_t my_signature[64];       // 64 Bytes (own cross-signature, R || S)
    uint8_t sig_len;                // 1 Byte (0 if not signed yet)
    uint8_t flags;                  // 1 Byte (0x01 = verified in person)
} gpg_recv_key_t;
```

**Maximum Keys:** 128. NVS itself imposes no hard limit; the cap exists so the
in-memory list buffer used by RECV_LIST stays predictable (allocated in PSRAM).

## API

### Storage (`GpgRecvStore`)

```cpp
class GpgRecvStore {
public:
    static GpgRecvStore& instance();

    // Persist a received key. Returns false if the store is full
    // (kMaxKeys == 128) or NVS write fails.
    bool addKey(const gpg_recv_key_t& key);

    // Number of stored keys.
    uint8_t count() const;

    // Load one key by list index (0 .. count()-1).
    bool getKey(uint8_t index, gpg_recv_key_t* out) const;

    // Remove one key by list index.
    bool deleteKey(uint8_t index);

    // Attach a cross-signature + flag bits to an existing key.
    bool setSignature(uint8_t index,
                      const uint8_t* sig, uint8_t sig_len,
                      uint8_t flags);
};
```

### Cross-Signing (`xsig.h`)

```cpp
// Hash to be signed: SHA-256(fingerprint_v4 || padded(user_id, 64))
bool gpgCrossSignDigest(const uint8_t fp_v4[20],
                        const char* user_id,
                        uint8_t out_hash[32]);

// Sign the target key with the badge's own SIG ECC slot.
// Output is 64 bytes (R || S) regardless of curve.
bool gpgCrossSign(const gpg_recv_key_t& target,
                  uint32_t sig_creation_time,
                  uint8_t out_sig[64]);

// Build an ASCII-armored OpenPGP block: Public Key + User ID + Cert Sig.
bool gpgBuildSignedKeyArmored(const gpg_recv_key_t& key,
                              char* out, size_t out_size,
                              size_t* out_len);
```

### BLE Exchange (`ble_gpg_xsig.h`)

```cpp
// Push own key to the connected peer; resolves when peer ACKs.
bool gpg_xsig_send(uint16_t conn_handle);

// Pull peer's key (server-initiated read of the data characteristic).
bool gpg_xsig_request(uint16_t conn_handle);

// Triggered when a remote frame finished assembly. On success
// the key has already been written through GpgRecvStore::addKey().
typedef void (*gpg_xsig_received_cb_t)(const gpg_recv_key_t* key);
void gpg_xsig_set_received_callback(gpg_xsig_received_cb_t cb);
```

## Serial Commands

The `GPG` command group (`RECV_LIST`, `RECV_INFO`, `CROSS_SIGN`, `RECV_DELETE`,
`EXPORT_SIGNED`) is documented in [Serial Commands](SERIAL_COMMANDS.md). All
entries require an authenticated session.

## Security

### Prerequisites

- BLE Secure Connections enabled
- Numeric Comparison for pairing
- User must confirm pairing

### Verification

Cross-signing should only occur after personal verification:

1. Compare fingerprints (display on both badges)
2. Verify name/email
3. Then sign

### Fingerprints

The badge calculates and stores both formats:

| Version | Hash | Length | Standard | Usage |
|---------|------|--------|----------|-------|
| V4 | SHA-1 | 20 Bytes (40 Hex) | RFC 4880 | GnuPG 2.x |
| V5 | SHA-256 | 32 Bytes (64 Hex) | RFC 9580 | GnuPG 2.5+ |

Both fingerprints are computed during key generation (own key) and on receive
(remote key) and stored in `gpg_recv_key_t`. V4 is the one used in the
cross-signature digest for GnuPG 2.x compatibility.

### OpenPGP Export

Cross-signed keys can be exported in RFC 4880 format:

```bash
# Via Serial Command
GPG EXPORT_SIGNED <index>

# Output: ASCII-armored OpenPGP
-----BEGIN PGP PUBLIC KEY BLOCK-----
...
-----END PGP PUBLIC KEY BLOCK-----
```

The export format contains:
- Public Key Packet (Tag 6, V4)
- User ID Packet (Tag 13)
- Certification Signature (Tag 2, Type 0x10)

Import into GnuPG:
```bash
gpg --import exported_key.asc
```

## Workflow (User Perspective)

### Send Key

1. GPG Menu → **Send Key**
2. Badge searches for other badges
3. Select target
4. Confirm BLE pairing
5. Exchange runs automatically

### View Received Keys

1. GPG Menu → **Received Keys**
2. Browse list
3. Select key for details

### Sign Key

1. Select received key in list
2. Select **Sign**
3. Compare fingerprint with owner
4. Confirm

## Compatibility

- **Badge-to-Badge:** Fully supported
- **With GnuPG 2.x:** Public keys and cross-signatures exportable as RFC 4880 packets
- **With GnuPG 2.5+:** V5 fingerprints (SHA-256) prepared
- **BLE Protocol:** Badge-specific GATT protocol (no standard BLE profile for GPG keys)

## Technical Details

### MPI Encoding (RFC 4880 Section 3.2)

Multi-Precision Integers are encoded with a leading bit count:
- Ed25519: 256 or 255 bits (depending on MSB)
- P-256: 520 bits (04 || X || Y = 65 Bytes × 8)

### Signature Semantics

| Algorithm | OID | Signing |
|-----------|-----|---------|
| EdDSA (Ed25519) | 1.3.6.1.4.1.11591.15.1 | Hash as "Message" |
| ECDSA (P-256) | 1.2.840.10045.3.1.7 | Hash directly |

Both produce 64-byte signatures (R || S, 32 bytes each).
