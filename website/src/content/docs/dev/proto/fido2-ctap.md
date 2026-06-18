---
title: FIDO2 / CTAP details
description: The verifiable CTAP protocol surface of the badge - commands, ClientPIN, COSE algorithms, credProtect, sign counters and attestation format.
sidebar:
  order: 11
---

This page documents the FIDO2 / CTAP protocol surface the badge actually
implements, as found in `components/mod_fido2/`. It is descriptive of the
firmware, not of the FIDO specification: where a feature is advertised but not
implemented, it is called out.

For the generated code reference, see the
[Code reference](/api/).

## Transport (CTAPHID)

| Property | Value |
| --- | --- |
| Transport | USB HID, FIDO Alliance usage page (CTAPHID) |
| Report / packet size | 64 bytes |
| CTAPHID commands handled | `INIT`, `PING`, `WINK`, `CANCEL`, `CBOR`, `MSG` |
| Capability flags (INIT) | `WINK \| CBOR` |
| Max message size constant | 2048 bytes |

`MSG` carries CTAP1 / U2F APDUs (`VERSION`, `REGISTER`, `AUTHENTICATE`); `CBOR`
carries CTAP2 commands.

## authenticatorGetInfo

The `getInfo` response is a 12-entry map:

| Key | Field | Value |
| --- | --- | --- |
| 0x01 | versions | `FIDO_2_0`, `FIDO_2_1`, `U2F_V2` |
| 0x02 | extensions | `appid`, `credProtect`, `appidExclude` |
| 0x03 | aaguid | `CDCBAD6E39C30001BAD6E00100000001` |
| 0x04 | options | see below |
| 0x05 | maxMsgSize | 1200 |
| 0x06 | pinUvAuthProtocols | `[2]` |
| 0x07 | maxCredentialCountInList | 8 |
| 0x08 | maxCredentialIdLength | 64 |
| 0x09 | transports | `["usb"]` |
| 0x0A | algorithms | ES256, EdDSA |
| 0x0B | maxSerializedLargeBlobArray | 1024 |
| 0x0D | minPINLength | current floor (default 4) |

### Options

Keys are emitted in CTAP canonical order (by length, then bytewise).

| Option | Value | Meaning |
| --- | --- | --- |
| `rk` | true | Resident / discoverable keys supported |
| `up` | true | User presence supported |
| `uv` | false | No built-in user verification (e.g. biometric) |
| `plat` | false | Removable authenticator |
| `alwaysUv` | current state | Whether every operation requires user verification |
| `credMgmt` | true | Credential management supported |
| `authnrCfg` | true | authenticatorConfig supported |
| `clientPin` | true | ClientPIN supported |
| `largeBlobs` | true | authenticatorLargeBlobs supported |
| `pinUvAuthToken` | true | pinUvAuthToken supported |
| `setMinPINLength` | true | setMinPINLength supported |
| `makeCredUvNotRqd` | true | makeCredential allowed without user verification |

## Supported CTAP2 commands

| Command | Code | Status |
| --- | --- | --- |
| makeCredential | 0x01 | Implemented |
| getAssertion | 0x02 | Implemented |
| getInfo | 0x04 | Implemented |
| clientPIN | 0x06 | Implemented |
| reset | 0x07 | Implemented (requires on-device user presence) |
| getNextAssertion | 0x08 | Implemented |
| credentialManagement | 0x0A | Implemented |
| selection | 0x0B | Implemented (user presence only) |
| largeBlobs | 0x0C | Implemented |
| authenticatorConfig | 0x0D | Implemented (toggleAlwaysUv, setMinPINLength) |
| bioEnrollment | 0x09 | **Not dispatched** (`CTAP1_ERR_INVALID_COMMAND`) |

:::caution[bioEnrollment is not implemented]
The badge reports version `FIDO_2_1` but has no biometric sensor, so
BioEnrollment (0x09) is not handled. Do not rely on it.
:::

## COSE algorithms and curves

| Algorithm | COSE id | Curve | Used for |
| --- | --- | --- | --- |
| ES256 | -7 | P-256 (secp256r1) | Credential keys, attestation |
| EdDSA | -8 | Ed25519 | Credential keys |
| ECDH-ES + HKDF-256 | -25 | P-256 | ClientPIN key agreement |

makeCredential selects ES256 -> P-256 or EdDSA -> Ed25519 from
`pubKeyCredParams`; any other requested algorithm yields
`CTAP2_ERR_UNSUPPORTED_ALGORITHM`.

## ClientPIN

| Aspect | Value |
| --- | --- |
| pinUvAuthProtocol | 2 (protocol 0 / unset also accepted) |
| Key agreement | COSE EC2, P-256, alg ECDH-ES+HKDF-256 |
| Protocol-2 encryption | AES-256-CBC, 16-byte IV prefixed to ciphertext |
| pinUvAuthParam | HMAC-SHA-256(pinToken, message), first 32 bytes (protocol 2) |
| PIN retries (max) | 8 |
| UV retries (max) | 3 |

Subcommands:

| Subcommand | Code | Status |
| --- | --- | --- |
| getPINRetries | 0x01 | Implemented |
| getKeyAgreement | 0x02 | Implemented |
| setPIN | 0x03 | **`CTAP2_ERR_UNSUPPORTED_OPTION`** |
| changePIN | 0x04 | **`CTAP2_ERR_UNSUPPORTED_OPTION`** |
| getPinToken | 0x05 | Implemented |
| getPinUvAuthTokenUsingPinWithPermissions | 0x09 | Implemented |

The PIN is **not** settable over the wire. The badge verifies the decrypted PIN
hash against its own stored PIN hash, so the FIDO PIN is the badge PIN, set
on-device. `getPinToken` returns `CTAP2_ERR_PIN_NOT_SET` when no PIN hash is
available and `CTAP2_ERR_PIN_BLOCKED` after the retry counter reaches zero.

### pinUvAuthToken permissions

The permission bits are defined (`mc`, `ga`, `cm`, `be`, `lbw`, `acfg`). The
credential-bound paths consume `mc`/`ga`/`cm`; `lbw` is required for
authenticatorLargeBlobs writes and `acfg` for authenticatorConfig.

:::caution[bioEnrollment permission has no consumer]
`be` (bioEnrollment) exists as a constant only; the badge has no biometric
sensor, so granting that permission has no effect.
:::

## makeCredential behaviour

- `up` must be true (false yields `CTAP2_ERR_INVALID_OPTION`); `uv=true` is
  rejected with `CTAP2_ERR_UNSUPPORTED_OPTION` (no internal UV).
- When `alwaysUv` is enabled (see authenticatorConfig), a request without a
  verified `pinUvAuthParam` is rejected with `CTAP2_ERR_PIN_REQUIRED`.
- User presence is always requested on the device before a key is created.
- An existing credential for the same RP-ID + user handle is overwritten.
- `appidExclude` matching an existing credential yields
  `CTAP2_ERR_CREDENTIAL_EXCLUDED`.
- Attestation is always returned (see below).

### credProtect

The `credProtect` extension (levels 1-3) is parsed at registration, stored with
the credential, echoed in the authenticator-data extensions, and reported by
credential management (defaulting to level 1 when unset).

:::caution[credProtect not enforced]
`credProtect` is recorded and reported but **not enforced** at assertion time.
Credential selection in `getAssertion` does not hide level-3 credentials when
user verification has not been performed. Treat credProtect on this badge as
advisory metadata, not an access control.
:::

## getAssertion and sign counters

- With no `allowList`, all resident credentials for the RP are returned
  (discoverable flow), with `getNextAssertion` iterating the rest.
- authData flags: `UP` (0x01) is set when user presence was requested; `UV`
  (0x04) is set when a pinUvAuth token was verified for the request.
- When `alwaysUv` is enabled, an assertion without a verified pinUvAuth token is
  rejected with `CTAP2_ERR_PIN_REQUIRED`.
- ECDSA assertions are DER-encoded; EdDSA assertions are raw 64-byte
  signatures, both produced by the secure element.

### Signature counter

Each credential has its **own** monotonic signature counter, incremented on
every assertion and persisted in TROPIC01 R-Memory. (A separate global
authentication counter is also kept in NVS, distinct from the per-credential
counter reported in authData.)

## Credential management

- Requires a valid `pinUvAuthToken` before any subcommand runs.
- Implemented subcommands: `getCredsMetadata`, `enumerateRPsBegin/GetNext`,
  `enumerateCredentialsBegin/GetNext`, `deleteCredential`.
- Responses include `credProtect` for each credential.

:::caution[Partial PIN-auth verification]
The per-subcommand `pinUvAuthParam` HMAC is not re-verified; access is gated only
by overall pinUvAuthToken validity. `updateUserInformation` (0x07) is not
implemented and falls through to `CTAP2_ERR_UNSUPPORTED_OPTION`.
:::

## Large blobs (0x0C)

The badge implements `authenticatorLargeBlobs` as a single serialized large-blob
array.

- The array is stored in NVS, capped at **1024 bytes**
  (`maxSerializedLargeBlobArray`). An unset store reads back as the canonical
  empty array (`0x80` followed by the left 16 bytes of `SHA-256(0x80)`).
- `get` returns a fragment from `offset` (no PIN required).
- `set` writes fragments in order: the first fragment (offset 0) carries the
  total `length`; out-of-order offsets yield `CTAP1_ERR_INVALID_SEQ`, a total
  above the cap yields `CTAP2_ERR_LARGE_BLOB_STORAGE_FULL`. When a PIN is set,
  each write must carry a `pinUvAuthParam` with the `lbw` permission over
  `0xff x 32 || 0x0c || 0x00 || offset (LE32) || SHA-256(fragment)`.
- On the final fragment the trailing 16-byte truncated SHA-256 checksum is
  verified; a mismatch yields `CTAP2_ERR_INTEGRITY_FAILURE` and the write is
  discarded.

## authenticatorConfig (0x0D)

Supported subcommands:

| Subcommand | Code | Status |
| --- | --- | --- |
| toggleAlwaysUv | 0x02 | Implemented |
| setMinPINLength | 0x03 | Implemented |
| enableEnterpriseAttestation | 0x01 | **`CTAP2_ERR_UNSUPPORTED_OPTION`** |
| vendorPrototype | 0xFF | **`CTAP2_ERR_UNSUPPORTED_OPTION`** |

When a PIN is set, the command requires a `pinUvAuthParam` with the `acfg`
permission over `0xff x 32 || 0x0d || subCommand || subCommandParams`.

- **toggleAlwaysUv** flips a persistent flag. While set, every makeCredential and
  getAssertion without a verified pinUvAuth token is rejected with
  `CTAP2_ERR_PIN_REQUIRED`. The current value is reported as the `alwaysUv`
  getInfo option.
- **setMinPINLength** raises the minimum badge-PIN length floor (reported as the
  `minPINLength` getInfo value). The new value must be between the current floor
  and 8 (the badge PIN maximum); a lower or larger value yields
  `CTAP1_ERR_INVALID_PARAMETER`. The floor is enforced on the next on-device PIN
  change and persists across reboots. The `minPinLengthRPIDs` list and
  `forceChangePin` flag are accepted (covered by the auth) but not acted upon;
  the `minPinLength` makeCredential extension is not reported to RPs.

## Attestation format

makeCredential returns `packed` attestation with an `x5c` certificate array, a
single self-signed per-device certificate, and an ES256 (P-256) signature over
`authData || clientDataHash`. The attestation private key is a chip-bound key in
TROPIC01 ECC slot 0. See
[FIDO2 attestation key & AAGUID](/security/attestation/) for the
certificate fields and key lifecycle.

## Capacity

| Limit | Value | Source |
| --- | --- | --- |
| FIDO2 ECC key slots | 26 (slots 5-30) | `main/tropic_slot_map.h` |
| FIDO2 R-Memory slots | 27 (slots 5-31) | `main/tropic_slot_map.h` |
| Max credentials | 26 (one ECC slot each) | bounded by ECC slot count |
| Credential ID length | 64 bytes | `FIDO2_CRED_ID_LEN` |

Every credential, resident or not, consumes one ECC slot because its private key
is generated and held in the secure element; there is no key-wrapping scheme for
unlimited server-side credentials.
