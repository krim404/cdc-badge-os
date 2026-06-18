---
title: Backup container format
description: The exact on-disk layout of the .cdcbak encrypted backup container - magic, header, KDF, cipher, AAD - and the structure of the plaintext JSON payload.
sidebar:
  order: 12
---

This page documents the `.cdcbak` backup container produced by the badge and
reproduced byte-identically by `tools/backup.py`. The user-facing flow is
covered in [/guide/backup-restore/](/guide/backup-restore/).

The canonical implementation is `components/cdc_os_ui/src/BackupManager.cpp`. The
off-device tool in `tools/backup.py` is kept byte-for-byte compatible with it.

## On-disk encoding

The binary container is stored **base64-encoded** as the file `backup.cdcbak` on
the badge's `plugins` storage partition, so it is text-safe for transfer over
the serial link. Decode the base64 to obtain the binary container described
below.

## Binary container layout

All multi-byte integers are little-endian.

| Field | Size (bytes) | Value |
| --- | --- | --- |
| `magic` | 6 | ASCII `"CDCBAK"` |
| `version` | 1 | `0x01` |
| `kdf_iters` | 4 | PBKDF2 iteration count, uint32 LE |
| `salt` | 16 | PBKDF2 salt (random per export) |
| `nonce` | 12 | AES-GCM nonce / IV (random per export) |
| `ciphertext` | N | AES-256-GCM ciphertext of the JSON payload |
| `gcm_tag` | 16 | AES-GCM authentication tag |

The **header** is the first `6 + 1 + 4 + 16 + 12 = 39` bytes (everything up to
but excluding the ciphertext). The whole header is fed to AES-GCM as Additional
Authenticated Data (AAD), so magic, version, iteration count, salt and nonce are
all authenticated alongside the ciphertext.

`salt` and `nonce` are filled with hardware randomness at export
(`esp_fill_random`).

## Key derivation

The 32-byte AES key is derived from the user passphrase:

```
key = PBKDF2-HMAC-SHA256(passphrase, salt, kdf_iters)  ->  32 bytes
```

| Parameter | Value | Source |
| --- | --- | --- |
| KDF | PBKDF2-HMAC-SHA256 | `BackupManager.cpp:127-135` (`mbedtls_pkcs5_pbkdf2_hmac_ext`, `MBEDTLS_MD_SHA256`) |
| Iterations | 200000 | `BackupManager.cpp:49` (`KDF_ITERATIONS = 200000`) |
| Salt length | 16 bytes | `BackupManager.cpp:45` |
| Derived key length | 32 bytes | `BackupManager.cpp:48` |

The iteration count is read back from the header at import
(`BackupManager.cpp:211`), so the file is self-describing for the KDF cost; the
firmware still rejects any `version` byte other than `1`.

## Cipher

| Parameter | Value | Source |
| --- | --- | --- |
| Cipher | AES-256-GCM | `Crypto.h:54` (`mbedtls_gcm_setkey(..., MBEDTLS_CIPHER_ID_AES, key, 256)`) |
| Key size | 256-bit (32 bytes) | `BackupManager.cpp:48` |
| Nonce / IV | 12 bytes | `BackupManager.cpp:46` |
| Tag | 16 bytes | `BackupManager.cpp:47` |
| AAD | the 39-byte header | `BackupManager.cpp:170-171`, `221` |

The ciphertext and the 16-byte tag are stored separately in the container
(`ciphertext || gcm_tag`). The shared GCM helpers live in
`components/cdc_core/include/cdc_core/Crypto.h`
(`aesGcm256Seal` / `aesGcm256Open`).

A wrong passphrase or any tampering with the header or ciphertext fails the GCM
tag check, and the container does not decrypt.

## Plaintext payload (JSON)

The decrypted plaintext is a single unformatted JSON document. Top-level shape:

```json
{
  "host_api_level": "0.7",
  "fw_version": "0.6.6",
  "modules": {
    "mod_2fa": { },
    "mod_password": { },
    "mod_vcard": { }
  },
  "system": { }
}
```

- `host_api_level` is the build's host-API level string. On restore the file is
  rejected if its level is **newer** than the importing badge
  (`BackupManager.cpp:380-385`).
- `fw_version` is informational (the build's `APP_VERSION`).
- Each key under `modules` is a module's `getName()`. Only modules that opt in to
  backup emit a section; modules without `exportBackup` are simply absent. As of
  this writing only three modules contribute: `mod_2fa`, `mod_password`,
  `mod_vcard`.
- `system` carries OS-level settings that have no owning module.

Modules serialize **semantic records**, never raw secure-element slots or NVS
blobs, so a backup survives slot-layout and storage-format changes.

### `modules.mod_2fa`

```json
{
  "schema_ver": 1,
  "entries": [
    {
      "name": "...", "issuer": "...",
      "type": 0, "algorithm": 0, "digits": 6, "period": 30,
      "counter": 0, "flags": 0,
      "secret": "<Base32>"
    }
  ]
}
```

The OATH secret is exported as an unpadded Base32 string
(`TwoFaModule.cpp:1408-1424`). `type`, `algorithm` and `flags` are the module's
numeric enums.

### `modules.mod_password`

```json
{
  "schema_ver": 1,
  "entries": [
    {
      "title": "...", "username": "...", "password": "...",
      "url": "...", "notes": "...",
      "totp_slot": -1
    }
  ]
}
```

The password is stored in cleartext **inside the encrypted container**
(`PasswordModule.cpp:968`). `totp_slot` links the entry to a two-factor account.

### `modules.mod_vcard`

```json
{
  "schema_ver": 1,
  "own": "<vCard text>",
  "received": ["<vCard text>", "..."]
}
```

`own` is omitted when no own card is set; `received` is an array of full vCard
strings (`VcardModule.cpp:577-598`).

### `system`

```json
{
  "schema_ver": 1,
  "language": "de",
  "backlight": 0,
  "sleep_interval": 0,
  "tz_offset": 0,
  "badge_name": "...", "badge_info": "...", "badge_info2": "...",
  "wifi": {
    "ssid": "...", "pass": "...", "sec": 0, "dhcp": true,
    "ip": 0, "gw": 0, "nm": 0
  },
  "wifi_timeout": 0,
  "wifi_enabled": false,
  "modules_enabled": { "mod_2fa": true, "mod_password": true }
}
```

Optional string fields are present only when non-empty. The static-IP fields
(`ip` / `gw` / `nm`) are packed 32-bit values and are emitted only when
`dhcp` is `false`. `modules_enabled` maps each module's name to its enable state
(`SystemSettingsBackup.cpp:80-138`).

WiFi credentials, including the password, are part of this section. The container
is passphrase-encrypted, so the credentials are present only in encrypted form.

## Per-section schema versions

Each module section and the `system` section carry their own `schema_ver`
(currently `1`). On restore, a section whose `schema_ver` does not match the
importing build is skipped. There is no migration path: a breaking change bumps
`schema_ver` and the mismatching section is ignored.

## What is excluded

Secure-element private keys are **never** exported. The FIDO2 and GPG modules do
not implement `exportBackup`, so their keys (and any other TROPIC01 ECC-slot key
material) are absent from every backup. The badge UI and the serial `IMPORT`
command both state this explicitly.

## Restore semantics

- Best-effort: each record is restored independently; a per-record failure is
  counted but never aborts the restore (`IModule::BackupResult`).
- Upsert by identity: existing entries with the same identity (account name,
  password title, or exact vCard text) are overwritten by the backup.
- Sections for modules absent on the target badge are skipped and counted.
- Aggregate counts (imported / failed / modules / skipped / system) are returned
  to the caller and shown on-device.

## Off-device reproduction

`tools/backup.py` implements the same format with `hashlib.pbkdf2_hmac("sha256",
...)` and the `cryptography` package's `AESGCM`, using the 39-byte header as AAD
(`tools/backup.py:79-124`). `--decrypt` / `--encrypt` operate on the base64 file
form and need only the passphrase.
