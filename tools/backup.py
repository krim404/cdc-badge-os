#!/usr/bin/env python3
"""
CDC Badge OS - backup tool.

Drives the on-device BACKUP serial command, transfers the single encrypted
backup file over the VFAT serial shell, and can decrypt/encrypt the container
off-device with just the passphrase (same format the firmware uses).

Binary container format (BackupManager.cpp <-> here, byte-identical):

    magic[6]      "CDCBAK"
    version       1 byte   (== 1)
    kdf_iters     uint32   little-endian
    salt          16 bytes
    nonce         12 bytes
    ciphertext    N bytes  (AES-256-GCM of the JSON document)
    gcm_tag       16 bytes

    key = PBKDF2-HMAC-SHA256(passphrase, salt, kdf_iters) -> 32 bytes
    AES-256-GCM AAD = the header bytes (magic..nonce, i.e. everything before
    the ciphertext). Decrypting yields the plaintext JSON backup document.

    The binary container is stored base64-encoded on vFAT for text-safe serial
    transfer. --download / --upload / --decrypt / --encrypt all work with the
    base64-encoded form at the file boundary.

Modes:
    On-device trigger (serial, AUTH-gated):
        --export <pass>     BACKUP EXPORT  (badge writes /backup.cdcbak)
        --import <pass>     BACKUP IMPORT  (badge restores from /backup.cdcbak)
        --delete            BACKUP DELETE

    Transfer (VFAT serial shell):
        --download <out>    fetch /backup.cdcbak from the badge (base64 text)
        --upload <in>       stream a container to /backup.cdcbak (VFAT RECEIVE)

    Off-device crypto (no badge needed):
        --decrypt <in> --out <out.json> --pass <p>
        --encrypt <in.json> --out <out.cdcbak> --pass <p>

Examples:
    python tools/backup.py --export "correct horse" --pin 123456
    python tools/backup.py --download backup.cdcbak --pin 123456
    python tools/backup.py --decrypt backup.cdcbak --out backup.json --pass "correct horse"
    python tools/backup.py --upload backup.cdcbak --pin 123456
    python tools/backup.py --import "correct horse" --pin 123456

Required: pyserial (only for badge modes), cryptography (for crypto modes).
"""

import argparse
import binascii
import glob
import hashlib
import struct
import sys
import time
from pathlib import Path

SERIAL_BAUD = 115200
ACK_TIMEOUT_S = 5
END_TIMEOUT_S = 15
DEFAULT_CHUNK = 256

MAGIC = b"CDCBAK"
VERSION = 1
SALT_SIZE = 16
NONCE_SIZE = 12
TAG_SIZE = 16
KEY_SIZE = 32
KDF_ITERATIONS = 200000
HEADER_SIZE = len(MAGIC) + 1 + 4 + SALT_SIZE + NONCE_SIZE

DEVICE_FILE = "backup.cdcbak"


# --- Container crypto (no badge needed) ------------------------------------

def derive_key(passphrase, salt, iterations):
    return hashlib.pbkdf2_hmac("sha256", passphrase.encode("utf-8"), salt,
                               iterations, dklen=KEY_SIZE)


def encrypt_container(plaintext, passphrase, iterations=KDF_ITERATIONS):
    """Build a CDCBAK container from a plaintext JSON document."""
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        sys.exit("ERROR: install cryptography -> pip install cryptography")
    import os
    salt = os.urandom(SALT_SIZE)
    nonce = os.urandom(NONCE_SIZE)
    header = MAGIC + bytes([VERSION]) + struct.pack("<I", iterations) + salt + nonce
    key = derive_key(passphrase, salt, iterations)
    # AESGCM appends the 16-byte tag to the ciphertext, matching the firmware
    # layout (ciphertext || tag).
    ct_and_tag = AESGCM(key).encrypt(nonce, plaintext, header)
    return header + ct_and_tag


def decrypt_container(container, passphrase):
    """Recover the plaintext JSON document from a CDCBAK container."""
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        sys.exit("ERROR: install cryptography -> pip install cryptography")
    if len(container) < HEADER_SIZE + TAG_SIZE:
        raise ValueError("container too short")
    if container[:len(MAGIC)] != MAGIC:
        raise ValueError("bad magic (not a CDCBAK container)")
    version = container[len(MAGIC)]
    if version != VERSION:
        raise ValueError(f"unsupported container version {version}")
    off = len(MAGIC) + 1
    iterations = struct.unpack_from("<I", container, off)[0]
    off += 4
    salt = container[off:off + SALT_SIZE]
    off += SALT_SIZE
    nonce = container[off:off + NONCE_SIZE]
    off += NONCE_SIZE
    header = container[:HEADER_SIZE]
    ct_and_tag = container[HEADER_SIZE:]
    key = derive_key(passphrase, salt, iterations)
    return AESGCM(key).decrypt(nonce, ct_and_tag, header)


# --- Serial helpers (mirrors tools/upload.py) ------------------------------

def detect_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*", "COM*"):
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def open_port(args):
    port = args.port or detect_port()
    if not port:
        sys.exit("ERROR: no USB serial port detected, use --port")
    try:
        import serial
    except ImportError:
        sys.exit("ERROR: install pyserial -> pip install pyserial")
    p = serial.Serial(port, SERIAL_BAUD, timeout=ACK_TIMEOUT_S)
    time.sleep(0.2)
    p.reset_input_buffer()
    return p


def readline(p, timeout=ACK_TIMEOUT_S):
    p.timeout = timeout
    return p.readline().decode("utf-8", errors="replace").rstrip("\r\n")


def drain(p, quiet=0.3):
    old = p.timeout
    p.timeout = quiet
    while p.readline():
        pass
    p.timeout = old


def send_line(p, line):
    # Bare LF terminator: with CRLF the badge executes on \r and the trailing
    # \n is swallowed as the first payload byte of a streamed upload.
    p.write((line + "\n").encode("utf-8"))
    p.flush()


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


B64_ALPHABET = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                         b"abcdefghijklmnopqrstuvwxyz0123456789+/=")


def is_b64_line(s):
    """True if `s` (bytes) is non-empty and contains only base64 characters."""
    return bool(s) and all(c in B64_ALPHABET for c in s)


def authenticate(p, pin):
    if not pin:
        return
    send_line(p, f"AUTH {pin}")
    deadline = time.time() + 5
    while time.time() < deadline:
        resp = readline(p, timeout=1)
        if not resp:
            continue
        u = resp.upper()
        if "AUTHENTICATED" in u or u.startswith("OK"):
            return
        if "WRONG" in u or "LOCKED" in u or u.startswith("ERROR"):
            raise RuntimeError(f"AUTH failed: {resp}")
    raise RuntimeError("AUTH timed out (no OK response)")


def wait_for(p, prefixes, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        resp = readline(p, timeout=1)
        if not resp:
            continue
        s = resp.strip()
        while s.startswith(">"):
            s = s[1:].lstrip()
        for pref in prefixes:
            if s.startswith(pref):
                return s
    return None


def vfat_receive(p, relpath, data, progress=None):
    """Stream a file into the plugins partition via the VFAT serial shell."""
    total = len(data)
    p.reset_input_buffer()
    send_line(p, f"VFAT RECEIVE {relpath} {total} {crc32(data):08x}")
    ready = wait_for(p, ["READY", "ERR"], timeout=5)
    if ready is None:
        raise RuntimeError("no READY response from badge")
    if ready.startswith("ERR"):
        raise RuntimeError(f"badge refused upload: {ready}")
    sent = 0
    while sent < total:
        end = min(sent + DEFAULT_CHUNK, total)
        p.write(data[sent:end])
        p.flush()
        sent = end
        if progress:
            progress(sent / total)
    final = wait_for(p, ["OK", "ERR"], timeout=END_TIMEOUT_S)
    if not final or not final.startswith("OK"):
        raise RuntimeError(f"upload not finalised: {final!r}")
    return final


# --- Commands --------------------------------------------------------------

def cmd_trigger(args, sub, passphrase=None):
    p = open_port(args)
    authenticate(p, args.pin)
    line = f"BACKUP {sub}" + (f" {passphrase}" if passphrase else "")
    p.reset_input_buffer()
    send_line(p, line)
    resp = wait_for(p, ["OK", "ERROR"], timeout=END_TIMEOUT_S)
    print(resp or "(no response)")
    if not resp or not resp.startswith("OK"):
        sys.exit(1)


def cmd_upload(args):
    import base64
    path = Path(args.upload)
    if not path.is_file():
        sys.exit(f"ERROR: file not found: {path}")
    raw = path.read_bytes().strip()
    # The on-disk format is base64; decode to check the magic then re-encode
    # for upload (the badge expects the base64 text on vFAT).
    try:
        binary = base64.b64decode(raw)
    except Exception:
        sys.exit("ERROR: file is not valid base64 (not a .cdcbak file?)")
    if binary[:len(MAGIC)] != MAGIC:
        print("WARNING: decoded container does not start with the CDCBAK magic",
              file=sys.stderr)
    p = open_port(args)
    authenticate(p, args.pin)
    send_line(p, "VFAT CD /")
    drain(p)
    # Upload the base64 text (what the badge stores on vFAT).
    data = base64.b64encode(binary)
    print(f"Uploading -> /{DEVICE_FILE} ({path})")
    vfat_receive(p, DEVICE_FILE, data,
                 lambda f: print(f"  {f * 100:5.1f} %", end="\r"))
    print()


def cmd_download(args):
    p = open_port(args)
    authenticate(p, args.pin)
    send_line(p, "VFAT CD /")
    drain(p)
    p.reset_input_buffer()
    send_line(p, f"VFAT GET {DEVICE_FILE}")
    # The reply shares the CDC channel with the command echo, debug log lines
    # and the shell prompt; the payload itself is pure base64, so keep only
    # base64 lines and drop everything else.
    payload = b""
    p.timeout = 1.0
    while True:
        line = p.readline()
        if not line:
            break
        s = line.strip()
        while s.startswith(b">"):
            s = s[1:].lstrip()
        if s.startswith(b"ERR"):
            sys.exit(f"ERROR: {s.decode('utf-8', 'replace')}")
        if is_b64_line(s):
            payload += s
    if not payload:
        sys.exit("ERROR: no backup data received")
    Path(args.download).write_bytes(payload)
    print(f"Wrote {len(payload)} bytes to {args.download}")


def cmd_decrypt(args):
    import base64
    raw = Path(args.decrypt).read_bytes().strip()
    try:
        container = base64.b64decode(raw)
    except Exception:
        sys.exit("ERROR: input is not valid base64 (not a .cdcbak file?)")
    plaintext = decrypt_container(container, args.passphrase)
    Path(args.out).write_bytes(plaintext)
    print(f"Decrypted {len(plaintext)} bytes -> {args.out}")


def cmd_encrypt(args):
    import base64
    plaintext = Path(args.encrypt).read_bytes()
    container = encrypt_container(plaintext, args.passphrase)
    encoded = base64.b64encode(container)
    Path(args.out).write_bytes(encoded)
    print(f"Encrypted -> {args.out} ({len(encoded)} bytes base64)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--pin", help="Badge PIN for AUTH (FEATURE_SECURE_SERIAL)")
    ap.add_argument("--pass", dest="passphrase", help="Backup passphrase (crypto modes)")
    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument("--export", metavar="PASS", help="Trigger BACKUP EXPORT on the badge")
    grp.add_argument("--import", dest="import_", metavar="PASS",
                     help="Trigger BACKUP IMPORT on the badge")
    grp.add_argument("--delete", action="store_true", help="Trigger BACKUP DELETE")
    grp.add_argument("--upload", metavar="FILE", help="Upload a container to the badge")
    grp.add_argument("--download", metavar="OUT", help="Download the badge container")
    grp.add_argument("--decrypt", metavar="FILE", help="Decrypt a container off-device")
    grp.add_argument("--encrypt", metavar="JSON", help="Encrypt a JSON file off-device")
    ap.add_argument("--out", help="Output path (for --decrypt / --encrypt)")
    args = ap.parse_args()

    if args.export:
        cmd_trigger(args, "EXPORT", args.export)
    elif args.import_:
        cmd_trigger(args, "IMPORT", args.import_)
    elif args.delete:
        cmd_trigger(args, "DELETE")
    elif args.upload:
        cmd_upload(args)
    elif args.download:
        cmd_download(args)
    elif args.decrypt:
        if not args.out or not args.passphrase:
            ap.error("--decrypt requires --out and --pass")
        cmd_decrypt(args)
    elif args.encrypt:
        if not args.out or not args.passphrase:
            ap.error("--encrypt requires --out and --pass")
        cmd_encrypt(args)


if __name__ == "__main__":
    main()
