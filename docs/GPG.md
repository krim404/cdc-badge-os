# GPG / OpenPGP Smartcard

The CDC Badge implements the OpenPGP 3.4 smartcard application over USB CCID. It works end-to-end with GnuPG on Linux, macOS, and Windows for signing, verification, encryption, decryption, and SSH authentication.

## What runs where

The TROPIC01 secure element supports ECDSA (P-256) and EdDSA (Ed25519) signing in hardware, but does **not** support ECDH. The badge fills that gap with a hybrid architecture: keys that need ECDH (the DEC role) live wrapped in TROPIC01 R-Memory and are decrypted on-the-fly by the ESP32; the chip-bound wrap key cannot be regenerated without the same TROPIC01 instance.

### TROPIC01 (hardware-side)

| Function | Slot | Used for |
|----------|------|----------|
| EdDSA-Sign Ed25519 | ECC 1, 3 | PSO:CDS (SIG), INTERNAL_AUTHENTICATE (AUT) |
| ECDSA-Sign P-256 | ECC 1, 3 | dito when host selects P-256 |
| ECC Key generation (TRNG-backed) | ECC 1, 3 | GENERATE_KEYPAIR for SIG + AUT |
| Public key export | ECC 0–31 | Read-pub for every role |
| ECDSA-Sign on attestation slot | ECC 0 | Signs PIN blob (PinManager) and OpenPGP NVS state |
| Hardware TRNG | global | Nonces, salts, reset-confirmation tokens |
| R-Memory R/W | RMEM 0–511 | Persistent storage backbone |
| Bus locking + session management | — | Serialises concurrent access from FIDO2, BLE, GPG |

Private key material for SIG and AUT never leaves the secure element.

### ESP32 mbedtls (software-side)

| Function | Why on host | Privkey location |
|----------|-------------|------------------|
| ECDH P-256 (PSO:DECIPHER) | TROPIC01 has no ECDH | 32-byte buffer in internal DRAM, unwrapped on use, immediately zeroized |
| DEC privkey wrap storage | Tropic stores no plaintext ECDH key | AES-256-GCM blob in R-Mem slot 2 (64 bytes total) |
| AES-256-GCM wrap/unwrap | Tropic has no AEAD primitive | mbedtls, 12-byte nonce from `getRandomStrict`, 16-byte tag |
| HKDF-SHA256 storage-key derivation | per-slot wrap key | IKM derived from TROPIC01 chip-bound material |
| Iterated S2K (PIN, RC, OpenPGP hashes) | OpenPGP spec | analogous to PinManager |
| ECDSA-Verify (P-256) for NVS-state signature | Host verifies Tropic-produced sig | mbedtls_ecdsa_verify against attestation pubkey |
| Card application logic, APDU parser, T=1 block protocol, CCID class driver | reine Anwendungs- und Transportschicht | `components/mod_gpg/` |

### What it looks like in practice

`gpg --sign` (handled entirely in hardware):

```
gpg → PSO:CDS(hash)
  ↳ ESP32 OpenPGP app routes APDU
    ↳ TROPIC01: lt_ecc_eddsa_sign(slot=1, hash)   # hardware
  ← signature → APDU response
```

`gpg --decrypt` (the hybrid path):

```
gpg → PSO:DECIPHER(peer-pubkey)
  ↳ ESP32 OpenPGP app
    ↳ TROPIC01: rmemRead slot 2 (encrypted DEC blob, 64 B)
    ↳ mbedtls: HKDF over chip-bound material → AES key
    ↳ mbedtls: AES-GCM-decrypt → 32-byte plaintext privkey in DRAM
    ↳ mbedtls: ECDH P-256 (privkey × peer-pubkey)
    ↳ mbedtls_platform_zeroize over privkey buffer
  ← shared secret → APDU response
```

## Card identification

| Property | Value |
|----------|-------|
| Manufacturer ID | 0x4344 (`"CD"`, displayed as `unknown` by gpg) |
| Serial Number | Derived from ESP32 MAC address |
| AID | `D2 76 00 01 24 01 03 04 43 44 <serial> 00 00` |
| VID:PID | 0x08E6:0x4433 (Gemalto IDBridge K30 compatible — bypasses libccid whitelist) |
| Card capability bits | T=1, extended-length APDUs, GET CHALLENGE |

## Key roles and curves

| Role | Tag | Slot | Default curve | Supports |
|------|-----|------|---------------|----------|
| SIG (Signature) | 0xB6 | ECC 1 | Ed25519 | Ed25519, P-256 ECDSA |
| DEC (Decryption) | 0xB8 | ECC 2 (logical, software) | P-256 ECDH | P-256 ECDH only (Tropic constraint) |
| AUT (Authentication) | 0xA4 | ECC 3 | Ed25519 | Ed25519, P-256 ECDSA |

The host can switch SIG/AUT between Ed25519 and P-256 ECDSA via `PUT DATA C1`/`C3`. DEC is hard-wired to P-256 ECDH (RSA and X25519 are not supported).

## Storage map

```
TROPIC01 ECC slots:
  Slot 0  : attestation key (system / PinManager)
  Slot 1  : SIG (signature key, Ed25519 or P-256, hardware-only)
  Slot 2  : DEC (placeholder; the actual ECDH private key lives in R-Mem 2)
  Slot 3  : AUT (authentication key, Ed25519 or P-256, hardware-only)

TROPIC01 R-Memory (slots 1-31 follow the convention: RMEM N is the metadata /
companion slot for ECC slot N):
  Slot 0  : PIN payload + ECDSA attestation signature (PinManager)
  Slot 1  : reserved (ECC 1 SIG companion - unused; SIG needs no software metadata)
  Slot 2  : DEC privkey, AES-256-GCM wrapped
            (4 magic "ECDH" + 12 nonce + 32 ciphertext + 16 GCM tag = 64 B)
  Slot 3  : AES symmetric key for PSO:ENCIPHER / PSO:DECIPHER 0x02
            (4 magic "AES1" + 12 nonce + (1 len + 32 key) + 16 GCM tag = 65 B)

ESP32 NVS namespace "openpgp":
  Key "state"  : OpenpgpNvsState payload + 64-byte ECDSA signature.
                 Holds fingerprints, gen-times, signature counter, cardholder
                 data, RC salt + hash + retry counter. Signed with the TROPIC01
                 attestation slot; a tampered blob is detected on boot and
                 triggers re-initialisation to defaults.

  Future: public keys received from other badges (cross-signing) belong in
  this namespace too - they are not secret and do not need TROPIC01 R-Memory.
```

## End-to-end usage

The badge needs three things on the host side to integrate with gpg + SSH:

1. **`gpg-agent.conf`** with `enable-ssh-support` and a `pinentry-program` that can show a dialog (e.g. `pinentry-mac` on macOS, `pinentry-gtk2` on Linux).
2. **`sshcontrol`** containing the keygrip of the card's AUT subkey.
3. **`SSH_AUTH_SOCK`** pointing at the gpg-agent SSH socket, not the OS default (relevant on macOS, less so on Linux).

### Bootstrap

```bash
# 1. Initial generation (do this exactly once per card serial)
gpg --card-edit
# admin
# generate
# Make off-card backup of encryption key? n     <-- important: do not pick y unless you set a passphrase
# Key valid forever? y
# Real name: <your name>
# Email: <optional>
# Comment: <optional>
# o (Okay)
# Wait for "public and secret key created and signed."

# 2. Find the AUT subkey keygrip
gpg --list-keys --with-keygrip <primary-fingerprint>
# Use the keygrip of the [A]-flagged subkey

# 3. Wire SSH up
echo "<AUT-keygrip>" >> ~/.gnupg/sshcontrol
echo "enable-ssh-support" >> ~/.gnupg/gpg-agent.conf
echo "pinentry-program $(which pinentry-mac)" >> ~/.gnupg/gpg-agent.conf     # macOS
gpgconf --kill gpg-agent
gpg-connect-agent /bye

# 4. Set SSH_AUTH_SOCK for the current shell
export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket)"
# Persist in ~/.zshrc:
echo 'export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket)"' >> ~/.zshrc

# 5. Verify
ssh-add -L     # must show "ssh-ed25519 ... cardno:<serial>"
ssh <host>     # first time: pinentry-mac asks for PW1, subsequent calls use cache (default 600 s)
```

### Sign + verify

```bash
echo "hello" > /tmp/m.txt
gpg --sign /tmp/m.txt          # triggers PSO:CDS on the card
gpg --verify /tmp/m.txt.gpg    # pure host-side EdDSA verify
```

### Encrypt + decrypt

```bash
gpg --encrypt --recipient <your-key-id> --trust-model always -o /tmp/m.enc.gpg /tmp/m.txt
gpg --decrypt -o /tmp/m.dec.txt /tmp/m.enc.gpg
diff /tmp/m.txt /tmp/m.dec.txt
```

The decrypt step is where the ESP32 ECDH path runs. Expect a one-off pinentry prompt for PW1 if the cache has expired.

## Serial commands

Group command: `GPG <subcommand> [args]`. All entries require an authenticated session.

| Sub-command | Description |
|-------------|-------------|
| `GPG STATUS` | Show keys, fingerprints, counters |
| `GPG GENERATE <curve> <user_id>` | Generate SIG + DEC + AUT in one shot (curve 1 = Ed25519 for SIG/AUT, 2 = P-256 ECDSA for SIG/AUT; DEC is always P-256 ECDH) |
| `GPG EXPORT` | Print primary + subkey pubkeys as PEM |
| `GPG RESET [token]` | Two-step destructive reset (see below) |
| `GPG RECV_LIST` / `RECV_INFO <i>` / `RECV_DELETE <i>` | Inspect / remove received cross-sign keys — see [CROSS_SIGNING.md](CROSS_SIGNING.md) |
| `GPG CROSS_SIGN <i>` | Cross-sign a received key |
| `GPG EXPORT_SIGNED <i>` | ASCII-armored OpenPGP export of a cross-signed key |

### Reset workflow

`GPG RESET` wipes all three ECC slots, the DEC backup in R-Mem slot 2, the AES key in slot 3, and resets PINs to factory defaults. To prevent fat-fingered loss, the command is two-step:

```text
> GPG RESET
WARNING: this wipes ALL GPG keys (SIG/DEC/AUT), the DEC backup, and PINs.
Confirm within 30s: GPG RESET A4F921

> GPG RESET A4F921
OK
```

The token is regenerated each time and only valid for 30 seconds. The same reset is also reachable from the GPG menu on the device (with on-screen confirmation) and via the OpenPGP `TERMINATE_DF` + `ACTIVATE_FILE` APDU pair triggered by `gpg --card-edit -> factory-reset`.

## Security model

### What is hardware-secured

- **SIG / AUT private keys** live exclusively in TROPIC01 ECC slots. Every signature is computed by the TROPIC01 chip. Flash dump alone is not enough to forge signatures; only physical possession of the secure element (and breaking it) would compromise these keys.
- **PIN material and OpenPGP state** are signed with the attestation key in slot 0 before being persisted. A tampered flash image is detected on boot and the affected store is reinitialised to defaults.

### What is in software

- **DEC private key**: TROPIC01 cannot perform ECDH, so the private key has to be available in plaintext during decryption. It is stored AES-256-GCM-encrypted in R-Mem slot 2; the wrap key is derived from TROPIC01-resident chip-bound material via HKDF. The plaintext exists in DRAM only for the duration of the ECDH computation and is zeroized immediately.
- **AES PSO key**: same construction as the DEC privkey, used for OpenPGP-style symmetric encryption (PSO:ENCIPHER / PSO:DECIPHER aes-128/256).

### Threat model summary

| Tier | Attacker has | Recoverable secret? |
|------|--------------|---------------------|
| T1 | USB only | None (PIN brute-force limited to 3 retries; counter is pre-decremented before the compare, so power-cycle attacks don't reset the counter) |
| T2 | Stolen badge, can dump flash, no chip-decap | SIG/AUT keys safe (in TROPIC01). DEC ciphertext readable but useless without TROPIC01 chip — wrap key is chip-bound. PIN hashes salted + S2K-iterated. |
| T3 | T2 + working attack on TROPIC01 | All keys reachable. The badge does not claim resistance to this — it is a DIY-grade hardware key, not a CC-EAL-certified smartcard. |

### Realistic limits

- Anything that depends on live glitching while the card is actively performing crypto is **out of scope**: an attacker with that level of access already has the operation's result. The badge does not attempt to defend against this.
- The badge is most robust as a **signing key + SSH authentication device**. Decryption works correctly, but is a software ECDH and has the same attack surface as any software OpenPGP key — except that the wrap binds the secret to this specific TROPIC01.

## Limitations

| Limitation | Reason |
|------------|--------|
| No RSA | TROPIC01 has no RSA primitives, ESP32-side RSA via mbedtls would push DEC blobs into multi-slot wrap territory and is not implemented |
| No X25519 / Curve25519 ECDH | Tropic constraint, DEC is P-256 only |
| No additional ECDH curves (P-384, P-521, secp256k1) | Same |
| Off-card backup of DEC during `generate` requires a passphrase | This is GnuPG's host-side behaviour, not a card limitation — answer `n` to "Make off-card backup of encryption key?" to keep the key card-only |

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `gpg --card-status`: `General key info..: [none]` | Generate flow aborted before `public and secret key created and signed.` was printed | Run `gpg --card-edit → admin → generate → n` again, all the way through |
| `ssh-add -L`: `The agent has no identities.` | `SSH_AUTH_SOCK` points at OS-native ssh-agent, or `sshcontrol` is missing the AUT keygrip | See bootstrap section above |
| `ssh ...`: `agent refused operation` | `pinentry-program` missing or pinentry can't open a dialog from the SSH context | Configure `pinentry-program` and `export GPG_TTY=$(tty); gpg-connect-agent updatestartuptty /bye` |
| `read_public_key DEC: load_dec_privkey failed` in serial log | R-Mem slot 2 was written by an older firmware version with a different wrap-key derivation | Run `gpg --card-edit → admin → generate` to rewrite the slot with the current scheme |
| Serial console silent on boot | Release build (`DEBUG_MODE=0`); console is auth-gated until `auth <pin>` | Authenticate, or build with `DEBUG_MODE=1` (the default for development) |

## Reference

- OpenPGP smartcard 3.4 specification (gnupg.org/ftp/specs/OpenPGP-smart-card-application-3.4.1.pdf)
- TROPIC01 datasheet for ECC/R-Memory primitives
- [CROSS_SIGNING.md](CROSS_SIGNING.md) — badge-to-badge GPG key exchange
- [SERIAL_COMMANDS.md](SERIAL_COMMANDS.md) — full serial command reference
