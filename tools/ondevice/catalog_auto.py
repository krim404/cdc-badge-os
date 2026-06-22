"""Fully automatic on-device tests.

Every test here is driven entirely over the serial console with no human
interaction, performs a round-trip, and cleans up after itself. State-mutating
or slow tests carry a flag so the default run stays fast and non-destructive;
they run only with --mutating / --slow.

All tests verify the installed release as-is. No firmware is flashed.
"""

import base64
import hashlib
import hmac
import os
import re
import struct
import time

from .link import (BadgeSerial, SkipTest, Test, find_line,
                   joined, match_line, require)

_B64 = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")


def _totp(secret_b32: str, when: int, digits: int = 8, period: int = 30,
          algo: str = "sha1") -> str:
    """RFC 6238 TOTP, computed on the host to compare against the badge."""
    pad = "=" * (-len(secret_b32) % 8)
    key = base64.b32decode(secret_b32 + pad, casefold=True)
    msg = struct.pack(">Q", when // period)
    digest = hmac.new(key, msg, getattr(hashlib, algo)).digest()
    off = digest[-1] & 0x0F
    code = (struct.unpack(">I", digest[off:off + 4])[0] & 0x7FFFFFFF) % (10 ** digits)
    return str(code).zfill(digits)


def _download_b64(b: BadgeSerial, fname: str) -> str:
    """Fetch a base64 text file from the plugins partition via VFAT GET.

    Retries once: a preceding flash-heavy command (e.g. a GPG reset) can leave
    the device briefly busy so the first GET returns before the body arrives."""
    b.command("VFAT CD /")
    for _ in range(2):
        lines = b.command(f"VFAT GET {fname}", timeout=10, quiet=0.8)
        out = [line for line in lines
               if len(line) >= 24 and all(c in _B64 for c in line.encode("utf-8"))]
        if out:
            return "".join(out)
        b.drain()
    return ""


# OpenPGP Ed25519 public-key packet body, encoded exactly as the firmware does
# (algo 22, curve OID, MPI without the 0x40 native-point prefix, 255/256 bits).
_OID_ED25519 = bytes([0x09, 0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01])


def _ed25519_fp_v4(pubkey: bytes, created: int) -> bytes:
    """Reproduce the badge's V4 fingerprint for an Ed25519 public point.

    The point is encoded as the OpenPGP native-format MPI: 0x40 prefix + 32-byte
    point = 263 bits (RFC 9580)."""
    mpi = bytes([0x01, 0x07, 0x40]) + pubkey
    body = bytes([0x04]) + struct.pack(">I", created) + bytes([22]) + _OID_ED25519 + mpi
    prefix = bytes([0x99, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
    return hashlib.sha1(prefix + body).digest()


def _gpg_key_payload(pubkey: bytes, created: int, fp20: bytes, uid: bytes) -> bytes:
    """Build a GpgKeyPayload wire blob (curve Ed25519=0)."""
    return (bytes([0, 32]) + pubkey + struct.pack(">I", created) + fp20
            + bytes([len(uid)]) + uid)


def _hex_lines(data: bytes, width: int = 64) -> list:
    h = data.hex()
    return [h[i:i + width] for i in range(0, len(h), width)]


def _dearmor(text: str) -> bytes:
    """Strip an ASCII-armored OpenPGP block down to its binary packets."""
    out, inblk = [], False
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("-----BEGIN"):
            inblk = True
            continue
        if line.startswith("-----END"):
            break
        if not inblk or not line or ":" in line or line.startswith("="):
            continue
        out.append(line)
    return base64.b64decode("".join(out)) if out else b""


def _packet_tags(data: bytes) -> list:
    """Return the OpenPGP packet tags in a binary blob (best-effort walk)."""
    tags, i, n = [], 0, len(data)
    while i < n:
        ctb = data[i]
        if not (ctb & 0x80):
            break
        if ctb & 0x40:  # new format
            tag = ctb & 0x3F
            i += 1
            if i >= n:
                break
            b0 = data[i]
            if b0 < 192:
                plen, i = b0, i + 1
            elif b0 < 224:
                plen, i = ((b0 - 192) << 8) + data[i + 1] + 192, i + 2
            elif b0 == 255:
                plen, i = struct.unpack(">I", data[i + 1:i + 5])[0], i + 5
            else:
                break
        else:  # old format
            tag, lt = (ctb >> 2) & 0x0F, ctb & 0x03
            i += 1
            if lt == 0:
                plen, i = data[i], i + 1
            elif lt == 1:
                plen, i = struct.unpack(">H", data[i:i + 2])[0], i + 2
            elif lt == 2:
                plen, i = struct.unpack(">I", data[i:i + 4])[0], i + 4
            else:
                break
        tags.append(tag)
        i += plen
    return tags


# --- tests -----------------------------------------------------------------

def test_sys(b: BadgeSerial) -> None:
    require(find_line(b.command("PING"), "PONG"), "PING did not return PONG")
    require(find_line(b.command("STATUS"), "Uptime"), "STATUS missing uptime")
    require(b.command("MEM"), "MEM returned nothing")
    require(find_line(b.command("CPU", timeout=6), "CPU"), "CPU load not reported")
    ml = b.command("MODULE LIST", timeout=8)
    require(ml and not find_line(ml, "Unknown command"), f"MODULE LIST: {joined(ml)}")


def test_pin(b: BadgeSerial) -> None:
    st = b.command("PIN STATUS")
    line = find_line(st, "retries=")
    require(line, f"PIN STATUS failed: {joined(st)}")
    require("set=yes" in line, "no badge PIN set; cannot test PIN change")
    cur = b.pin
    require(cur, "PIN change test needs --pin")
    temp = "47318" if cur != "47318" else "47319"
    c1 = b.command(f"PIN CHANGE {cur} {temp}")
    require(find_line(c1, "OK: PIN changed"), f"PIN CHANGE failed: {joined(c1)}")
    try:
        a = b.command(f"AUTH {temp}")
        require(find_line(a, "Authenticated") or find_line(a, "OK"),
                f"new PIN did not authenticate: {joined(a)}")
    finally:
        c2 = b.command(f"PIN CHANGE {temp} {cur}")
        require(find_line(c2, "OK: PIN changed"),
                f"PIN restore failed: {joined(c2)} -- PIN may still be {temp}")
        b.command(f"AUTH {cur}")


def test_lockout(b: BadgeSerial) -> None:
    require(b.pin, "lockout test needs --pin")
    b.command("LOGOUT")
    for _ in range(4):
        b.command("AUTH 99999999")
    r = b.command(f"AUTH {b.pin}")
    require(not (find_line(r, "Authenticated") or find_line(r, "OK: Auth")),
            f"not locked out, correct PIN accepted: {joined(r)}")
    b.note("waiting ~65 s for the lockout to recover...")
    time.sleep(65)
    r = b.command(f"AUTH {b.pin}")
    require(find_line(r, "Authenticated") or find_line(r, "OK"),
            f"recovery failed after lockout: {joined(r)}")


def test_2fa(b: BadgeSerial) -> None:
    secret, name = "JBSWY3DPEHPK3PXP", "HRNStest"
    host_now = int(time.time())
    r = b.command(f"SET_DATE {host_now}")
    require(find_line(r, "OK: Time set"), f"SET_DATE failed: {joined(r)}")
    a = b.command(f'TOTP ADD totp {name} {secret} "" 8 30 sha1')
    require(find_line(a, "OK"), f"TOTP ADD failed: {joined(a)}")
    lst = b.command("TOTP LIST")
    m = match_line(lst, rf"^(\d+):\s*{re.escape(name)}\b")
    require(m, f"added entry not listed: {joined(lst)}")
    idx = m.group(1)
    try:
        g = b.command(f"TOTP GET {idx}")
        cm = match_line(g, r"\b(\d{8})\b")
        require(cm, f"no 8-digit code in: {joined(g)}")
        code = cm.group(1)
        now = int(time.time())
        accepted = {_totp(secret, now + d) for d in (-30, 0, 30)}
        accepted |= {_totp(secret, host_now + d) for d in (-30, 0, 30)}
        require(code in accepted,
                f"TOTP mismatch: device={code} not in {sorted(accepted)}")
    finally:
        b.command(f"TOTP DEL {idx}")


def test_password(b: BadgeSerial) -> None:
    r = b.command("PASSWORD ADD x HRNStitle hrnsuser hrnspass x -")
    m = match_line(r, r"OK \(slot (\d+)\)")
    require(m, f"PASSWORD ADD failed: {joined(r)}")
    slot = m.group(1)
    try:
        g = b.command(f"PASSWORD GET {slot}")
        require(find_line(g, "Title: HRNStitle"), f"title round-trip: {joined(g)}")
        require(find_line(g, "Username: hrnsuser"), f"user round-trip: {joined(g)}")
        require(find_line(g, "Password: hrnspass"), f"password round-trip: {joined(g)}")
        e = b.command(f"PASSWORD EDIT {slot} username hrnsuser2")
        require(find_line(e, "OK"), f"PASSWORD EDIT failed: {joined(e)}")
        g2 = b.command(f"PASSWORD GET {slot}")
        require(find_line(g2, "Username: hrnsuser2"), f"edit not applied: {joined(g2)}")
    finally:
        b.command(f"PASSWORD DEL {slot}")


def test_vcard(b: BadgeSerial) -> None:
    orig = b.command("VCARD GET")
    had_own = any("BEGIN:VCARD" in line for line in orig)
    body = ["BEGIN:VCARD", "VERSION:4.0", "FN:HRNS Test", "ORG:Harness", "END:VCARD"]
    r = b.paste("VCARD SET", body)
    require(find_line(r, "OK"), f"VCARD SET failed: {joined(r)}")
    try:
        g = b.command("VCARD GET")
        require(find_line(g, "FN:HRNS Test"), f"vCard round-trip failed: {joined(g)}")
    finally:
        if had_own:
            keep = [l for l in orig if l and not l.startswith("ERROR")]
            b.paste("VCARD SET", keep)
        else:
            b.command("VCARD DELETE")


def _gpg_reset(b: BadgeSerial) -> None:
    """Two-step destructive GPG reset: capture the random token and confirm it."""
    r = b.command("GPG RESET", timeout=8)
    m = match_line(r, r"GPG RESET ([0-9A-Fa-f]{4,8})")
    if m:
        b.command(f"GPG RESET {m.group(1)}", timeout=8)


def test_gpg(b: BadgeSerial) -> None:
    s = b.command("GPG STATUS", timeout=8)
    if find_line(s, "User-ID:"):
        raise SkipTest("a GPG key is already present; refusing to overwrite an identity")
    b.command("GPG GENERATE 1 harness@test", timeout=30)
    s2 = b.command("GPG STATUS", timeout=8)
    require(find_line(s2, "User-ID:"), f"no key after GENERATE: {joined(s2)}")
    e = b.command_until("GPG EXPORT", "END PGP", timeout=10)
    require(find_line(e, "BEGIN PGP PUBLIC KEY"), f"no armored key in EXPORT: {joined(e)}")
    require(find_line(e, "END PGP"), f"EXPORT armored block truncated: {joined(e)}")
    _gpg_reset(b)


def _backup_cmd(b: BadgeSerial, cmd: str, timeout: float = 40) -> str:
    """Run a backup command whose PBKDF2 (200k iters, ~18 s) outlasts the quiet
    window; wait for the terminating OK/ERROR line instead of quiet detection."""
    b.drain()
    b.send(cmd)
    line = b.wait_for(["OK", "ERROR", "FAIL"], timeout=timeout)
    require(line is not None, f"{cmd}: no OK/ERROR within {timeout:.0f}s")
    return line


def test_backup(b: BadgeSerial) -> None:
    passphrase = "HarnessBackupPass123"
    e = _backup_cmd(b, f"BACKUP EXPORT {passphrase}")
    require(e.startswith("OK"), f"BACKUP EXPORT failed: {e}")
    try:
        payload = _download_b64(b, "backup.cdcbak")
        require(payload, "no backup payload downloaded over VFAT")
        raw = base64.b64decode(payload)
        require(raw[:6] == b"CDCBAK", f"bad container magic: {raw[:6]!r}")
        require(raw[6] == 1, f"unexpected container version {raw[6]}")
        require(len(raw) >= 6 + 1 + 4 + 16 + 12 + 16, "container shorter than header+tag")
        w = _backup_cmd(b, f"BACKUP IMPORT WRONG{passphrase}")
        require(not w.startswith("OK"), f"wrong passphrase not rejected: {w}")
        i = _backup_cmd(b, f"BACKUP IMPORT {passphrase}")
        require(i.startswith("OK"), f"BACKUP IMPORT failed: {i}")
    finally:
        _backup_cmd(b, "BACKUP DELETE")


def test_lang(b: BadgeSerial) -> None:
    info = b.command("LANG INFO", timeout=6)
    require(info and not find_line(info, "Unknown command"), f"LANG INFO failed: {joined(info)}")
    r = b.command("LANG RELOAD", timeout=8)
    require(find_line(r, "OK"), f"LANG RELOAD failed: {joined(r)}")


def test_settings(b: BadgeSerial) -> None:
    t = b.command("GET_TIME")
    require(match_line(t, r"\b\d{2}:\d{2}:\d{2}\b"), f"GET_TIME format: {joined(t)}")
    s = b.command("SET_TIME 12:34:56")
    require(find_line(s, "OK: Time set"), f"SET_TIME failed: {joined(s)}")
    t2 = b.command("GET_TIME")
    require(match_line(t2, r"\b12:34:\d{2}\b"), f"time not reflected: {joined(t2)}")
    n = b.command("NVS LIST", timeout=8)
    require(n and not find_line(n, "Unknown command"), f"NVS LIST failed: {joined(n)}")
    # Leave a sane wall clock.
    b.command(f"SET_DATE {int(time.time())}")


def test_wifi(b: BadgeSerial) -> None:
    st = b.command("WIFI STATUS", timeout=6)
    require(st and not find_line(st, "Unknown command"), f"WIFI STATUS failed: {joined(st)}")
    was_off = bool(find_line(st, "off")) or bool(find_line(st, "OFF"))
    on = b.command("WIFI ON", timeout=8)
    require(find_line(on, "OK"), f"WIFI ON failed: {joined(on)}")
    sc = b.command("WIFI SCAN", timeout=15, quiet=0.8)
    require(sc and not find_line(sc, "ERROR"), f"WIFI SCAN failed: {joined(sc)}")
    if was_off:
        b.command("WIFI OFF")


def test_tr01(b: BadgeSerial) -> None:
    s = b.command("TR01 STATUS", timeout=8)
    require(find_line(s, "Session:"), f"TR01 STATUS failed: {joined(s)}")
    if not find_line(s, "active"):
        b.note("TR01 session reported inactive")
    # TR01 SLOTS prints an ECC section, a blank line, then an R-Memory section;
    # read past the blank line until the R-Memory header rather than stopping.
    sl = b.command_until("TR01 SLOTS", "R-Memory Slots", timeout=8)
    require(find_line(sl, "ECC Key Slots"), f"TR01 SLOTS missing ECC section: {joined(sl)}")
    require(find_line(sl, "R-Memory Slots"), f"TR01 SLOTS missing R-Mem section: {joined(sl)}")


def test_attest(b: BadgeSerial) -> None:
    try:
        import datetime

        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.x509.oid import NameOID
    except ImportError:
        raise SkipTest("python-cryptography not installed")

    r = b.command("ATTEST EXPORT", timeout=6)
    m = match_line(r, r"\b([0-9A-Fa-f]{130})\b")
    require(m, f"ATTEST EXPORT gave no 65-byte pubkey: {joined(r)}")
    badge_pub = ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(), bytes.fromhex(m.group(1)))

    def make_cert(subject_pub) -> bytes:
        ca = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"CDC Harness CA")])
        return (x509.CertificateBuilder()
                .subject_name(name).issuer_name(name)
                .public_key(subject_pub).serial_number(1)
                .not_valid_before(datetime.datetime(2020, 1, 1))
                .not_valid_after(datetime.datetime(2040, 1, 1))
                .sign(ca, hashes.SHA256())
                .public_bytes(serialization.Encoding.DER))

    # Negative: a cert whose subject key is NOT the badge's must be rejected
    # (proves the firmware verifies the cert binds to slot-0, not blind-stores).
    other_pub = ec.generate_private_key(ec.SECP256R1()).public_key()
    neg = b.paste("ATTEST IMPORT", _hex_lines(make_cert(other_pub)), timeout=8)
    require(not find_line(neg, "imported"),
            f"mismatched attestation cert was accepted: {joined(neg)}")

    # Positive: a cert over the badge's own attestation key is accepted.
    try:
        pos = b.paste("ATTEST IMPORT", _hex_lines(make_cert(badge_pub)), timeout=8)
        require(find_line(pos, "imported") or find_line(pos, "OK"),
                f"valid attestation cert rejected: {joined(pos)}")
    finally:
        b.command("ATTEST CLEAR", timeout=6)


def test_gpg_xsign(b: BadgeSerial) -> None:
    s = b.command("GPG STATUS", timeout=8)
    generated = False
    if not find_line(s, "User-ID:"):
        b.command("GPG GENERATE 1 xsign@harness", timeout=30)
        s = b.command("GPG STATUS", timeout=8)
        require(find_line(s, "User-ID:"), f"no own key after GENERATE: {joined(s)}")
        generated = True

    pub, created, uid = os.urandom(32), 1700000000, b"PeerHarnessXSIG"
    payload = _gpg_key_payload(pub, created, _ed25519_fp_v4(pub, created), uid)
    imported = False
    try:
        # Negative: a payload whose fingerprint does not reproduce is rejected.
        bad = bytearray(payload)
        bad[38] ^= 0xFF  # flip first fingerprint byte (offset 1+1+32+4)
        neg = b.command("GPG RECV_IMPORT " + bad.hex(), timeout=8)
        require(not find_line(neg, "OK"),
                f"corrupted-fingerprint key was accepted: {joined(neg)}")

        # Positive: the firmware re-derives the V4 fingerprint and accepts it.
        pos = b.command("GPG RECV_IMPORT " + payload.hex(), timeout=8)
        require(find_line(pos, "OK"),
                f"valid peer key rejected (V4 fp replication?): {joined(pos)}")
        imported = True

        lst = b.command("GPG RECV_LIST", timeout=8)
        mi = match_line(lst, r"\[(\d+)\]\s*PeerHarnessXSIG")
        require(mi, f"imported peer key not listed: {joined(lst)}")
        idx = mi.group(1)

        # A received key exports without cross-signing, carrying the DEC
        # encryption subkey so a peer can encrypt to it.
        ex0 = b.command_until(f"GPG RECV_EXPORT {idx}", "END PGP", timeout=10)
        require(find_line(ex0, "BEGIN PGP PUBLIC KEY"),
                f"RECV_EXPORT produced no armored block: {joined(ex0)}")
        tags0 = _packet_tags(_dearmor(joined(ex0)))
        require(14 in tags0,
                f"no encryption subkey (tag 14) in unsigned export: {tags0}")

        cs = b.command(f"GPG RECV_CROSS_SIGN {idx}", timeout=10)
        require(find_line(cs, "OK"), f"RECV_CROSS_SIGN failed: {joined(cs)}")

        ex = b.command_until(f"GPG RECV_EXPORT {idx}", "END PGP", timeout=10)
        require(find_line(ex, "BEGIN PGP PUBLIC KEY"),
                f"RECV_EXPORT produced no armored block: {joined(ex)}")
        require(find_line(ex, "END PGP"),
                f"RECV_EXPORT armored block was truncated (no END): {joined(ex)}")
        tags = _packet_tags(_dearmor(joined(ex)))
        require(6 in tags, f"no public-key packet (tag 6) in export: {tags}")
        require(13 in tags, f"no user-id packet (tag 13) in export: {tags}")
        require(2 in tags, f"no certification signature packet (tag 2): {tags}")
        require(14 in tags, f"no encryption subkey (tag 14) in export: {tags}")
    finally:
        if imported:
            md = match_line(b.command("GPG RECV_LIST", timeout=8),
                            r"\[(\d+)\]\s*PeerHarnessXSIG")
            if md:
                b.command(f"GPG RECV_DELETE {md.group(1)}", timeout=8)
        if generated:
            _gpg_reset(b)


AUTO_TESTS = [
    Test("A-SYS", "System smoke", test_sys, frs="-", interaction="serial"),
    Test("A-PIN", "PIN change round-trip", test_pin, frs="FR-001/007",
         interaction="serial", flags=("mutating",)),
    Test("A-LOCK", "Serial-AUTH lockout + recovery", test_lockout, frs="FR-002/003",
         interaction="serial", flags=("slow",)),
    Test("A-2FA", "TOTP code round-trip", test_2fa, frs="FR-020/023", interaction="serial"),
    Test("A-PWD", "Password vault CRUD", test_password, frs="FR-030/032", interaction="serial"),
    Test("A-VCARD", "vCard storage round-trip", test_vcard, frs="FR-054", interaction="serial"),
    Test("A-GPG", "GPG key generate/export", test_gpg, frs="FR-040/044",
         interaction="serial", flags=("mutating",)),
    Test("A-BACKUP", "Backup export/import round-trip", test_backup, frs="FR-060/063",
         interaction="serial", flags=("mutating",)),
    Test("A-LANG", "i18n overlay reload", test_lang, frs="FR-100", interaction="serial"),
    Test("A-SET", "Settings / time / NVS", test_settings, frs="FR-091/094", interaction="serial"),
    Test("A-WIFI", "WiFi control", test_wifi, frs="FR-090", interaction="serial"),
    Test("A-TR01", "Secure-element health", test_tr01, frs="slot map", interaction="serial"),
    Test("A-ATTEST", "FIDO2 attestation cert import + key-match reject", test_attest,
         frs="FR-010/017", interaction="serial", flags=("mutating",)),
    Test("A-GPG-XSIGN", "GPG cross-sign received key (+fingerprint reject)", test_gpg_xsign,
         frs="FR-044", interaction="serial", flags=("mutating",)),
]
