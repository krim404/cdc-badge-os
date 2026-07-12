#!/usr/bin/env python3
"""
CDC Badge OS - Provisioning & Production Lockdown Tool

UNTESTED ON HARDWARE - DESTRUCTIVE. Never run against a badge you cannot
afford to lose.

This tool takes a CDC Badge (ESP32-S3 + TROPIC01) from the development/beta
state into a hardened production state. It performs partly IRREVERSIBLE
operations. It ships DISARMED: to run anything you MUST edit this file and set
ARMED = True (see the SAFETY LATCH below) AND export
CDC_PROVISION_EXPERIMENTAL=1 in the environment.

Subcommands
    rotate-key         Rotate the TROPIC01 SH0 sync (pairing) key away from the
                       public libtropic production key. Three staged phases:
                         soft       generate a new host keypair, write it into a
                                    free pairing slot, emit pairing_key_custom.h
                         --verify   prove the badge authenticates via the NEW
                                    slot (VERSION pairing_slot + TR01 SESSION)
                         invalidate PERMANENTLY kill the old pairing slot;
                                    refused until --verify succeeded
                       Requires a firmware built with -DFEATURE_PROVISIONING=1
                       (the TR01 PAIR_* serial commands do not exist otherwise).

    lock-esp           Enable Flash Encryption (release) + Secure Boot v2 on
                       the ESP32-S3 as a strict state machine. `lock-esp` shows
                       the plan and the chip's actual state; `lock-esp
                       --continue` executes exactly the NEXT open step - steps
                       can never be skipped or repeated. Order (keeps the badge
                       recoverable at every boundary):
                         1 generate FE key            (host)
                         2 generate SB key + digest   (host)
                         3 burn FE key                (free key block, verified)
                         4 burn SB digest             (free key block, verified)
                         5 sign+encrypt+flash release images (one write-flash)
                         6 burn SPI_BOOT_CRYPT_CNT + SECURE_BOOT_EN, then
                           verify the boot over serial (flash_enc=1 secure_boot=1)
                         7 harden (JTAG pad+USB, download caches, direct boot)

    reflash            Sign + pre-encrypt a new firmware build and flash it onto
                       an already-locked badge over serial (single write-flash).

    disable-download   Final download-mode lock. Refused until lock-esp is
                       complete and a verified boot is recorded. Two levels:
                         security    keep ROM download (encrypted writes only) -
                                     badge stays serially updatable (default)
                         permanent   burn DIS_DOWNLOAD_MODE - badge can NEVER be
                                     reflashed again (no OTA exists)

Per-chip state
    Everything lives in tools/secrets/<chip>/ (git-ignored, 0700):
      esp-<mac>/       fe_key.bin, sb_key.pem, sb_digest.bin, manifest.json
      tr01-<chipid>/   sync_key.json, manifest.json
    manifest.json binds chip identity, SHA-256 of key files, eFuse snapshots
    and completed steps together, so badges cannot be mixed up.

Examples
    python tools/provision.py rotate-key --dry-run
    python tools/provision.py rotate-key --pin 0000 --i-understand-this-is-irreversible
    python tools/provision.py rotate-key --verify --pin 0000
    python tools/provision.py rotate-key --invalidate-old-slot 0 --pin 0000 \\
        --i-understand-this-is-irreversible
    python tools/provision.py lock-esp --dry-run
    python tools/provision.py lock-esp --port /dev/ttyACM0            # status
    python tools/provision.py lock-esp --port /dev/ttyACM0 --continue \\
        --dir ./release-build --pin 0000 --i-understand-this-is-irreversible
    python tools/provision.py reflash --dir ./release-build --dry-run
    python tools/provision.py disable-download --mode security --dry-run

Notes
    - Order is mandatory: run rotate-key BEFORE lock-esp. The custom pairing
      key is compiled into the firmware and stays readable from a plaintext
      flash dump until flash encryption is enabled.
    - Release images come from `pio run -e cdc_badge_release`. NEVER upload
      that env manually: its bootloader self-burns eFuses on an unlocked chip.
    - --pin ends up in shell history and the process list; use a dedicated
      provisioning PIN and rotate it afterwards if that matters to you.
    - CLI syntax targets standalone esptool >= 5 (hyphenated commands); the
      version is verified before anything runs. The esptool bundled with
      ESP-IDF 5.5 (v4.x) is rejected.
    - Linux/macOS only (serial port auto-detection globs /dev).

Requires: pyserial, cryptography, esptool>=5,<6 (pip install -r tools/requirements.txt).
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

# ======================================================================
# SAFETY LATCH - this tool is DISARMED. It performs IRREVERSIBLE, UNTESTED,
# destructive operations (TROPIC01 pairing invalidation, eFuse burning,
# permanent flash lockout). To run it you MUST edit this file and set
# ARMED = True below AND export CDC_PROVISION_EXPERIMENTAL=1.
# ======================================================================
ARMED = False
EXPERIMENTAL_ENV = "CDC_PROVISION_EXPERIMENTAL"


def _require_armed():
    if not ARMED:
        sys.stderr.write(
            "\nREFUSED: provision.py is DISARMED.\n"
            "This tool does IRREVERSIBLE, UNTESTED, destructive things to a badge\n"
            "(pairing-slot invalidation, eFuse burning, permanent flash lockout).\n"
            "Open tools/provision.py and set  ARMED = True  to enable it.\n\n")
        sys.exit(1)
    if os.environ.get(EXPERIMENTAL_ENV) != "1":
        sys.stderr.write(
            "\nREFUSED: experimental opt-in missing.\n"
            "This tool has never been validated on real hardware. To acknowledge\n"
            f"that, additionally export  {EXPERIMENTAL_ENV}=1  in your shell.\n\n")
        sys.exit(1)


# --- paths -------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS_DIR = os.path.join(REPO_ROOT, "tools", "secrets")
PAIRING_HEADER = os.path.join(
    REPO_ROOT, "components", "cdc_hal", "include", "cdc_hal", "pairing_key_custom.h")

CHIP = "esp32s3"
SERIAL_BAUD = 115200
# Flash offsets (from partitions.csv / flash_firmware.py) -> (keyword, signed).
# SBv2 signs only bootloader + app. Signing the partition table would append a
# 4 KiB signature block and the write at 0x8000 would clobber NVS at 0x9000.
FLASH_MAP = {
    0x0: ("bootloader", True),
    0x8000: ("partitions", False),
    0x50000: ("firmware", True),
}
# Region limits derived from partitions.csv (next partition starts at these).
REGION_LIMIT = {0x0: 0x8000, 0x8000: 0x1000, 0x50000: 0xDA0000}

RED = "\033[1;31m"
YEL = "\033[1;33m"
RST = "\033[0m"


# --- console helpers ---------------------------------------------------------

def banner(lines):
    width = 70
    print(RED + "=" * width)
    for ln in lines:
        print(ln)
    print("=" * width + RST)


def info(msg):
    print(f"  {msg}")


def die(msg):
    sys.exit(f"ERROR: {msg}")


def _refuse_overwrite(path, what):
    """Existing key material is possibly the only copy - never clobber it."""
    if os.path.exists(path):
        die(f"{path} already exists ({what}).\n"
            "Refusing to overwrite: losing existing key material can permanently\n"
            "brick a badge. Move the file away manually if you really mean it.")


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


# --- per-chip secrets + manifest ----------------------------------------------

def _make_secrets_dir(subdir=None):
    path = os.path.join(SECRETS_DIR, subdir) if subdir else SECRETS_DIR
    os.makedirs(path, mode=0o700, exist_ok=True)
    os.chmod(path, 0o700)
    return path


def _manifest_path(chip_dir):
    return os.path.join(chip_dir, "manifest.json")


def load_manifest(chip_dir):
    try:
        with open(_manifest_path(chip_dir)) as f:
            return json.load(f)
    except FileNotFoundError:
        return {"keys": {}, "stages": {}, "pairing": {}}


def save_manifest(chip_dir, manifest):
    path = _manifest_path(chip_dir)
    with open(path, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    os.chmod(path, 0o600)


def _record_key(manifest, name, path):
    manifest["keys"][name] = {"file": os.path.basename(path),
                              "sha256": _sha256(path)}


def _check_key(manifest, name, path):
    """Die if a key file is missing or does not match the manifest hash."""
    rec = manifest["keys"].get(name)
    if not rec:
        die(f"manifest has no record of {name}; run the missing lock-esp step first")
    if not os.path.exists(path):
        die(f"{path} is missing but the manifest references it - restore it "
            "from backup, this badge cannot be reflashed without it")
    if _sha256(path) != rec["sha256"]:
        die(f"{path} does NOT match the manifest hash. Wrong badge directory "
            "or tampered/replaced key file - refusing to continue")


def _stamp(manifest, stage, **extra):
    manifest["stages"][stage] = dict(
        extra, done=True, at=time.strftime("%Y-%m-%dT%H:%M:%S"))


# --- serial link (self-contained, mirrors tools/ondevice/link.py) ------------

def detect_port():
    ports = []
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        ports.extend(glob.glob(pattern))
    if len(ports) > 1:
        # Never auto-pick between several badges for irreversible operations.
        die(f"multiple serial ports found ({', '.join(sorted(ports))}), use --port")
    return ports[0] if ports else None


class BadgeSerial:
    """Thin USB-CDC serial client for the badge console."""

    def __init__(self, port=None, pin=None):
        self.port = port
        self.pin = pin
        self._ser = None

    def connect(self):
        port = self.port or detect_port()
        if not port:
            die("no USB serial port detected, use --port")
        try:
            import serial
        except ImportError:
            die("install pyserial -> pip install pyserial")
        self._ser = serial.Serial(port, SERIAL_BAUD, timeout=5)
        self.port = port
        time.sleep(0.2)
        self._ser.reset_input_buffer()
        return self

    def close(self):
        if self._ser:
            self._ser.close()
            self._ser = None

    def send(self, line):
        # Bare LF: the badge executes on \r or \n and swallows a trailing \n.
        self._ser.write((line + "\n").encode("utf-8"))
        self._ser.flush()

    def collect(self, timeout=5, quiet=0.4):
        lines = []
        deadline = time.time() + timeout
        self._ser.timeout = quiet
        while time.time() < deadline:
            raw = self._ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
            if not raw:
                if lines:
                    break
                continue
            s = raw.strip().lstrip(">").strip()
            if s:
                lines.append(s)
        return lines

    def command(self, cmd, timeout=5):
        self._ser.reset_input_buffer()
        self.send(cmd)
        return self.collect(timeout=timeout)

    def authenticate(self):
        """AUTH <pin>; tolerates a build without FEATURE_SECURE_SERIAL."""
        if not self.pin:
            return
        for line in self.command(f"AUTH {self.pin}"):
            u = line.upper()
            if "AUTHENTICATED" in u or u.startswith("OK"):
                return
            if "UNKNOWN COMMAND" in u:
                return  # secure serial not compiled in
            if "WRONG" in u or "LOCKED" in u or u.startswith("ERROR"):
                die(f"AUTH failed: {line}")


def _expect_ok(resp, what):
    print("\n".join(f"  {l}" for l in resp))
    if not any(l.upper().startswith("OK") for l in resp):
        die(f"{what} did not return OK (see badge response above)")


def _parse_profile(lines):
    """Parse the VERSION 'Profile:' line into a dict of its key=value fields."""
    for line in lines:
        if line.lower().startswith("profile:"):
            return {k: v for k, v in
                    (tok.split("=", 1) for tok in line.split() if "=" in tok)}
    return None


def _chipid(badge):
    """Return the TROPIC01 chip id (hex) from TR01 INFO, or None."""
    for line in badge.command("TR01 INFO", timeout=8):
        m = re.search(r"chip id:\s*([0-9a-f]+)", line, re.IGNORECASE)
        if m:
            return m.group(1).upper()
    return None


# --- danger gate -------------------------------------------------------------

class DangerGate:
    """Multi-layer confirmation for an irreversible operation."""

    def __init__(self, args, dry_run):
        self.args = args
        self.dry_run = dry_run

    def confirm(self, title, consequences, phrase, extra_word=None, serial=None):
        """Return True if the operator has cleared every gate."""
        banner([f"  {title}",
                "  UNTESTED - DESTRUCTIVE - IRREVERSIBLE"] +
               [f"  - {c}" for c in consequences] +
               ([f"  Target badge serial: {serial}"] if serial else []))

        if self.dry_run:
            print(YEL + "  [dry-run] no changes will be made." + RST)
            return True

        if not getattr(self.args, "i_understand_this_is_irreversible", False):
            die("refused: pass --i-understand-this-is-irreversible to proceed")

        typed = input(f'\n  Type exactly to proceed:\n    {phrase}\n  > ').strip()
        if typed != phrase:
            die("confirmation phrase did not match; aborted")

        if extra_word:
            typed2 = input(f'  Second confirmation, type: {extra_word}\n  > ').strip()
            if typed2 != extra_word:
                die("second confirmation did not match; aborted")
        return True


# --- external tool resolution + preflight -------------------------------------

_TOOL_CACHE = {}


def _resolve_tool(name, verify=True):
    """Find an espressif tool as a CLI, verify it runs and is v5.x."""
    if name in _TOOL_CACHE:
        return _TOOL_CACHE[name]
    exe = shutil.which(name)
    prefix = [exe] if exe else [sys.executable, "-m", name]
    if verify:
        try:
            proc = subprocess.run(prefix + ["version"],
                                  capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as e:
            die(f"{name} not runnable ({e}); install it: "
                "pip install 'esptool>=5,<6'")
        if proc.returncode != 0:
            die(f"{name} not usable (exit {proc.returncode}); install it: "
                "pip install 'esptool>=5,<6'")
        m = re.search(r"v?(\d+)\.(\d+)", proc.stdout + proc.stderr)
        major = int(m.group(1)) if m else 0
        if not (5 <= major < 6):
            die(f"{name} reports version {m.group(0) if m else 'unknown'}; this "
                "script requires the hyphenated v5 CLI. Install: "
                "pip install 'esptool>=5,<6' (the ESP-IDF-bundled v4.x will not work)")
    _TOOL_CACHE[name] = prefix
    return prefix


def run_cmd(cmd, dry_run, why=""):
    """Print (and optionally run) an external command; die on failure."""
    printable = " ".join(cmd)
    tag = "[dry-run] " if dry_run else ""
    if why:
        info(f"# {why}")
    print(f"  {tag}$ {printable}")
    if dry_run:
        return ""
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        tail = out.strip().splitlines()[-6:]
        for ln in tail:
            print(f"    {ln}")
        die(f"command failed (exit {proc.returncode}): {printable}")
    return proc.stdout or ""


# --- eFuse state --------------------------------------------------------------

def efuse_summary(espefuse, port):
    """Read the chip's eFuse state as a dict (espefuse summary JSON)."""
    out = run_cmd(list(espefuse) + ["--chip", CHIP, "--port", port,
                                    "summary", "--format", "json"],
                  dry_run=False, why="read eFuse state")
    # espefuse may print banner lines before the JSON object.
    start = out.find("{")
    if start < 0:
        die("could not parse espefuse summary output (no JSON found)")
    try:
        return json.loads(out[start:])
    except json.JSONDecodeError as e:
        die(f"could not parse espefuse summary JSON: {e}")


def _ef(summary, name, default=None):
    entry = summary.get(name) or {}
    return entry.get("value", default)


def _esp_mac(summary):
    mac = str(_ef(summary, "MAC", "")).strip()
    mac = re.sub(r"\s*\(.*\)$", "", mac)  # strip '(OK)' style suffixes
    clean = mac.replace(":", "").lower()
    if not re.fullmatch(r"[0-9a-f]{12}", clean):
        die(f"could not read ESP MAC from eFuse summary (got {mac!r})")
    return clean


def _free_key_block(summary, purpose_needed):
    """Pick the first unused eFuse key block; die if none is free."""
    for n in range(6):
        purpose = str(_ef(summary, f"KEY_PURPOSE_{n}", ""))
        block = str(_ef(summary, f"BLOCK_KEY{n}", ""))
        if purpose == "USER" and set(block.replace(" ", "")) <= {"0"}:
            return n
    die(f"no free eFuse key block for {purpose_needed} - this chip already "
        "has all key blocks in use; refusing to guess")


def _purpose_block(summary, purpose):
    """Return the key-block index whose purpose matches, or None."""
    for n in range(6):
        if str(_ef(summary, f"KEY_PURPOSE_{n}", "")) == purpose:
            return n
    return None


# --- release artifact validation ----------------------------------------------

def _find_bin(directory, keyword):
    """Exactly one *.bin matching keyword; ambiguity is an error."""
    matches = [f for f in glob.glob(os.path.join(directory, "*.bin"))
               if keyword in os.path.basename(f).lower()
               and not f.endswith((".signed", ".enc"))]
    if not matches:
        die(f"no *{keyword}*.bin in {directory}")
    if len(matches) > 1:
        die(f"ambiguous {keyword} images in {directory}: "
            f"{', '.join(os.path.basename(m) for m in matches)}")
    return matches[0]


def _check_region(path, addr, what):
    size = os.path.getsize(path)
    limit = REGION_LIMIT[addr]
    if size > limit:
        die(f"{what} is {size} bytes but the region at {hex(addr)} only holds "
            f"{limit} bytes - it would overwrite the next partition")


def _prepare_images(args_dir, sb_key, fe_key, espsecure, dry):
    """Sign (bootloader+app only), size-check, verify, encrypt. Returns
    {addr: encrypted_path}."""
    out = {}
    for addr, (keyword, sign) in sorted(FLASH_MAP.items()):
        plain = _find_bin(args_dir, keyword)
        src = plain
        if sign:
            src = plain + ".signed"
            run_cmd(list(espsecure) + ["sign-data", "--version", "2",
                                       "--keyfile", sb_key, "--output", src, plain],
                    dry, why=f"sign {keyword}")
            if not dry:
                run_cmd(list(espsecure) + ["verify-signature", "--version", "2",
                                           "--keyfile", sb_key, src],
                        dry, why=f"verify {keyword} signature")
        else:
            info(f"# {keyword}: not signed (SBv2 covers bootloader + app only)")
        if not dry:
            _check_region(src, addr, f"{keyword} image ({os.path.basename(src)})")
        enc = plain + ".enc"
        run_cmd(list(espsecure) + ["encrypt-flash-data", "--aes-xts",
                                   "--keyfile", fe_key, "--address", hex(addr),
                                   "--output", enc, src],
                dry, why=f"encrypt {keyword} for offset {hex(addr)}")
        out[addr] = enc
    return out


def _flash_images(images, esptool_, port, dry):
    """Write all segments in ONE esptool invocation."""
    cmd = list(esptool_) + ["--chip", CHIP, "--port", port, "--no-stub",
                            "write-flash"]
    for addr, path in sorted(images.items()):
        cmd += [hex(addr), path]
    run_cmd(cmd, dry, why="flash all ciphertext segments in one pass")


# --- subcommand: rotate-key ----------------------------------------------------

def _gen_x25519():
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    priv = X25519PrivateKey.generate()
    priv_raw = priv.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption())
    pub_raw = priv.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw)
    return bytes(priv_raw), bytes(pub_raw)


def _c_array(raw):
    rows = []
    for i in range(0, len(raw), 16):
        chunk = raw[i:i + 16]
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(rows)


def _write_pairing_header(priv, pub, slot):
    body = f"""#pragma once

/**
 * \\file
 * \\brief Custom TROPIC01 SH0 pairing key (generated by tools/provision.py).
 *
 * Git-ignored. Never commit a real key. Build with:
 *   -DCDC_PAIRING_KEY=CDC_PAIRING_KEY_CUSTOM
 */

#include <stdint.h>

#include "libtropic_common.h"

static const uint8_t cdc_pairing_key_priv[32] = {{
{_c_array(priv)}
}};

static const uint8_t cdc_pairing_key_pub[32] = {{
{_c_array(pub)}
}};

/* Pairing-key slot the custom key occupies (0..3). */
#define CDC_PAIRING_KEY_SLOT TR01_PAIRING_KEY_SLOT_INDEX_{slot}
"""
    with open(PAIRING_HEADER, "w") as f:
        f.write(body)
    os.chmod(PAIRING_HEADER, 0o600)  # contains the private key


def _ensure_gitignore_secrets():
    gi = os.path.join(REPO_ROOT, ".gitignore")
    needle = "tools/secrets/"
    try:
        with open(gi) as f:
            if needle in f.read():
                return
    except FileNotFoundError:
        pass
    with open(gi, "a") as f:
        f.write(f"\n# Provisioning secrets (sync keys, FE/SB keys) - never commit\n{needle}\n")


def _tr01_dir(chip_id):
    return _make_secrets_dir(f"tr01-{chip_id.lower()}")


def cmd_rotate_key(args):
    dry = args.dry_run

    # Dry-run needs no hardware: show intent, touch nothing, connect nothing.
    if dry:
        if args.verify:
            print(YEL + "  [dry-run] would check: VERSION pairing_slot == "
                  "manifest slot, TR01 SESSION -> OK, TR01 INFO chip id == "
                  "manifest chip id; then record pairing.verified=true" + RST)
            return 0
        if args.invalidate_old_slot is not None:
            slot = args.invalidate_old_slot
            DangerGate(args, dry).confirm(
                title=f"INVALIDATE TROPIC01 PAIRING SLOT {slot}",
                consequences=[
                    f"Pairing slot {slot} becomes PERMANENTLY dead.",
                    "Invalidating all 4 slots bricks the secure element.",
                    "Refused unless rotate-key --verify recorded a working new slot."],
                phrase=f"INVALIDATE PAIRING SLOT {slot}")
            print(f"  [dry-run] would send: TR01 PAIR_INVALIDATE {slot} CONFIRM")
            return 0
        slot = args.new_slot
        priv, pub = _gen_x25519()
        print(YEL + "  [dry-run] generated a keypair (not persisted):" + RST)
        info(f"pub  = {pub.hex()}")
        info(f"slot = {slot}")
        info("would write: tools/secrets/tr01-<chipid>/sync_key.json + manifest.json")
        info(f"would write: {PAIRING_HEADER}")
        info(f"would send : TR01 PAIR_WRITE {slot} {pub.hex()}")
        return 0

    _ensure_gitignore_secrets()
    badge = BadgeSerial(args.port, args.pin).connect()
    try:
        badge.authenticate()
        chip_id = _chipid(badge)
        if not chip_id:
            die("could not read the TROPIC01 chip id (TR01 INFO) - without it "
                "keys cannot be bound to this badge")
        chip_dir = _tr01_dir(chip_id)
        manifest = load_manifest(chip_dir)
        manifest["tropic_chip_id"] = chip_id
        key_file = os.path.join(chip_dir, "sync_key.json")

        # --- verify phase: prove the new slot actually works -----------------
        if args.verify:
            pairing = manifest.get("pairing", {})
            if not pairing.get("written"):
                die("nothing to verify: no pairing key was written for this "
                    f"badge (chip {chip_id})")
            profile = _parse_profile(badge.command("VERSION", timeout=8))
            if not profile or "pairing_slot" not in profile:
                die("firmware does not report pairing_slot in VERSION - flash "
                    "a build with the extended Profile line first")
            if int(profile["pairing_slot"]) != int(pairing["slot"]):
                die(f"badge authenticates via slot {profile['pairing_slot']}, "
                    f"expected new slot {pairing['slot']} - rebuild/flash with "
                    "-DCDC_PAIRING_KEY=CDC_PAIRING_KEY_CUSTOM first")
            _expect_ok(badge.command("TR01 SESSION", timeout=10), "TR01 SESSION")
            live_id = _chipid(badge)
            if live_id != chip_id:
                die(f"chip id mismatch: badge reports {live_id}, manifest has "
                    f"{chip_id} - wrong badge connected?")
            manifest["pairing"]["verified"] = True
            _stamp(manifest, "pairing_verified", slot=int(pairing["slot"]))
            save_manifest(chip_dir, manifest)
            info(f"OK: new pairing slot {pairing['slot']} verified on chip {chip_id}")
            info("You may now invalidate the old slot (rotate-key "
                 "--invalidate-old-slot N).")
            return 0

        # --- invalidate phase -------------------------------------------------
        if args.invalidate_old_slot is not None:
            slot = args.invalidate_old_slot
            pairing = manifest.get("pairing", {})
            if not pairing.get("verified"):
                die("refused: the new pairing slot was never verified on this "
                    "badge. Run  rotate-key --verify  first (with the "
                    "custom-key firmware flashed)")
            profile = _parse_profile(badge.command("VERSION", timeout=8))
            active = int(profile["pairing_slot"]) if profile and \
                "pairing_slot" in profile else None
            if active is None:
                die("firmware does not report pairing_slot in VERSION - "
                    "refusing to invalidate blindly")
            if slot == active:
                die(f"refused: slot {slot} is the slot the badge is currently "
                    "authenticating with")
            DangerGate(args, dry).confirm(
                title=f"INVALIDATE TROPIC01 PAIRING SLOT {slot}",
                consequences=[
                    f"Pairing slot {slot} becomes PERMANENTLY dead.",
                    "Invalidating all 4 slots bricks the secure element.",
                    f"Badge authenticates via verified slot {active}."],
                phrase=f"INVALIDATE PAIRING SLOT {slot}",
                serial=chip_id)
            resp = badge.command(f"TR01 PAIR_INVALIDATE {slot} CONFIRM", timeout=10)
            _expect_ok(resp, f"PAIR_INVALIDATE {slot}")
            manifest.setdefault("pairing", {}).setdefault(
                "invalidated", []).append(slot)
            _stamp(manifest, f"pair_invalidate_{slot}")
            save_manifest(chip_dir, manifest)
            return 0

        # --- soft phase: generate + write new key into a free slot ----------
        _refuse_overwrite(key_file, "previous sync key")
        _refuse_overwrite(PAIRING_HEADER, "previous pairing key header")
        slot = args.new_slot
        priv, pub = _gen_x25519()
        record = {
            "curve": "x25519",
            "slot": slot,
            "priv_hex": priv.hex(),
            "pub_hex": pub.hex(),
            "chip": chip_id,
        }

        DangerGate(args, dry).confirm(
            title=f"WRITE NEW PAIRING KEY INTO SLOT {slot}",
            consequences=[
                f"Pairing slot {slot} can be written only ONCE.",
                f"The private key is saved to {os.path.relpath(key_file, REPO_ROOT)}.",
                "Loss of that file means the new key is unrecoverable."],
            phrase=f"WRITE PAIRING SLOT {slot}",
            serial=chip_id)

        with open(key_file, "w") as f:
            json.dump(record, f, indent=2)
        os.chmod(key_file, 0o600)
        _write_pairing_header(priv, pub, slot)
        manifest["pairing"] = {"slot": slot, "written": True, "verified": False}
        _record_key(manifest, "sync_key", key_file)
        _stamp(manifest, "pair_write", slot=slot)
        save_manifest(chip_dir, manifest)

        resp = badge.command(f"TR01 PAIR_WRITE {slot} {pub.hex()}", timeout=10)
        _expect_ok(resp, f"PAIR_WRITE {slot}")

        banner([
            "  NEXT STEPS",
            "  1. Rebuild + flash firmware with the new custom key:",
            "       PLATFORMIO_BUILD_FLAGS="
            "'-DCDC_PAIRING_KEY=CDC_PAIRING_KEY_CUSTOM' \\",
            "       pio run -e cdc_badge_usb -t upload",
            "  2. Verify the badge boots and the new slot works:",
            "       provision.py rotate-key --verify",
            f"  3. Only then invalidate the old slot:",
            "       provision.py rotate-key --invalidate-old-slot 0 ...",
        ])
        return 0
    finally:
        badge.close()


# --- subcommand: lock-esp (state machine) --------------------------------------

class Ctx:
    """Everything a lock-esp step needs."""

    def __init__(self, args, dry):
        self.args = args
        self.dry = dry
        self.port = args.port or detect_port() or "PORT"
        self.espefuse = _resolve_tool("espefuse", verify=not dry)
        self.espsecure = _resolve_tool("espsecure", verify=not dry)
        self.esptool = _resolve_tool("esptool", verify=not dry)
        self.summary = {}
        self.chip_dir = None
        self.manifest = {"keys": {}, "stages": {}}
        self.gate = DangerGate(args, dry)

    def path(self, name):
        return os.path.join(self.chip_dir, name)

    def refresh(self):
        self.summary = efuse_summary(self.espefuse, self.port)

    def load(self):
        self.refresh()
        mac = _esp_mac(self.summary)
        self.chip_dir = _make_secrets_dir(f"esp-{mac}")
        self.manifest = load_manifest(self.chip_dir)
        self.manifest["esp_mac"] = mac
        self.manifest["efuse_snapshot"] = {
            k: _ef(self.summary, k) for k in (
                "SPI_BOOT_CRYPT_CNT", "SECURE_BOOT_EN", "ENABLE_SECURITY_DOWNLOAD",
                "DIS_DOWNLOAD_MODE", "HARD_DIS_JTAG", "DIS_USB_JTAG")}

    def save(self):
        save_manifest(self.chip_dir, self.manifest)


HARDEN_EFUSES = ["DIS_DOWNLOAD_MANUAL_ENCRYPT", "DIS_DOWNLOAD_ICACHE",
                 "DIS_DOWNLOAD_DCACHE", "HARD_DIS_JTAG", "DIS_USB_JTAG",
                 "DIS_DIRECT_BOOT"]


def _step_gen_fe(ctx):
    fe = ctx.path("fe_key.bin")
    _refuse_overwrite(fe, "flash-encryption key")
    run_cmd(list(ctx.espsecure) + ["generate-flash-encryption-key", fe],
            ctx.dry, why="generate host flash-encryption key")
    if not ctx.dry:
        os.chmod(fe, 0o600)
        _record_key(ctx.manifest, "fe_key", fe)


def _step_gen_sb(ctx):
    sb = ctx.path("sb_key.pem")
    digest = ctx.path("sb_digest.bin")
    _refuse_overwrite(sb, "secure-boot signing key")
    run_cmd(list(ctx.espsecure) + ["generate-signing-key", "--version", "2",
                                   "--scheme", "rsa3072", sb],
            ctx.dry, why="generate secure-boot RSA-3072 signing key")
    run_cmd(list(ctx.espsecure) + ["digest-sbv2-public-key", "--keyfile", sb,
                                   "--output", digest],
            ctx.dry, why="compute SB public-key digest")
    if not ctx.dry:
        os.chmod(sb, 0o600)
        _record_key(ctx.manifest, "sb_key", sb)
        _record_key(ctx.manifest, "sb_digest", digest)


def _step_burn_fe(ctx):
    fe = ctx.path("fe_key.bin")
    _check_key(ctx.manifest, "fe_key", fe)
    block = _free_key_block(ctx.summary, "XTS_AES_128_KEY")
    ctx.gate.confirm(
        title=f"BURN FLASH-ENCRYPTION KEY INTO BLOCK_KEY{block}",
        consequences=["The key block becomes read+write protected forever.",
                      "eFuse bits can NEVER be un-burned."],
        phrase=f"BURN FE KEY BLOCK {block}",
        serial=ctx.manifest.get("esp_mac"))
    run_cmd(list(ctx.espefuse) + ["--chip", CHIP, "--port", ctx.port,
                                  "--do-not-confirm", "burn-key",
                                  f"BLOCK_KEY{block}", fe, "XTS_AES_128_KEY"],
            ctx.dry, why="burn FE key (read+write protected)")
    if not ctx.dry:
        ctx.refresh()
        if _purpose_block(ctx.summary, "XTS_AES_128_KEY") is None:
            die("read-back failed: no key block reports purpose XTS_AES_128_KEY "
                "after burning - do NOT continue, inspect the chip manually")
        _stamp(ctx.manifest, "burn_fe_key", block=block)


def _step_burn_sb(ctx):
    digest = ctx.path("sb_digest.bin")
    _check_key(ctx.manifest, "sb_digest", digest)
    block = _free_key_block(ctx.summary, "SECURE_BOOT_DIGEST0")
    ctx.gate.confirm(
        title=f"BURN SECURE-BOOT DIGEST INTO BLOCK_KEY{block}",
        consequences=["Only firmware signed with sb_key.pem will boot after "
                      "SECURE_BOOT_EN is set.",
                      "eFuse bits can NEVER be un-burned."],
        phrase=f"BURN SB DIGEST BLOCK {block}",
        serial=ctx.manifest.get("esp_mac"))
    run_cmd(list(ctx.espefuse) + ["--chip", CHIP, "--port", ctx.port,
                                  "--do-not-confirm", "burn-key",
                                  f"BLOCK_KEY{block}", digest,
                                  "SECURE_BOOT_DIGEST0"],
            ctx.dry, why="burn SB public-key digest (stays readable)")
    if not ctx.dry:
        ctx.refresh()
        if _purpose_block(ctx.summary, "SECURE_BOOT_DIGEST0") is None:
            die("read-back failed: no key block reports purpose "
                "SECURE_BOOT_DIGEST0 after burning - do NOT continue")
        _stamp(ctx.manifest, "burn_sb_digest", block=block)


def _step_flash_release(ctx):
    if not ctx.args.dir:
        die("this step flashes the release images: pass --dir <release-build> "
            "(from `pio run -e cdc_badge_release`)")
    sb = ctx.path("sb_key.pem")
    fe = ctx.path("fe_key.bin")
    _check_key(ctx.manifest, "sb_key", sb)
    _check_key(ctx.manifest, "fe_key", fe)
    images = _prepare_images(ctx.args.dir, sb, fe, ctx.espsecure, ctx.dry)
    _flash_images(images, ctx.esptool, ctx.port, ctx.dry)
    if not ctx.dry:
        _stamp(ctx.manifest, "flash_release",
               images={hex(a): _sha256(p) for a, p in images.items()})


def _step_enable(ctx):
    ctx.gate.confirm(
        title="ENABLE FLASH ENCRYPTION + SECURE BOOT V2 (FINAL)",
        consequences=[
            "SPI_BOOT_CRYPT_CNT=7 and SECURE_BOOT_EN=1 are burned together.",
            "From the next boot only the encrypted, signed images run.",
            "eFuse bits can NEVER be un-burned."],
        phrase="ENABLE LOCKDOWN NOW",
        serial=ctx.manifest.get("esp_mac"))
    run_cmd(list(ctx.espefuse) + ["--chip", CHIP, "--port", ctx.port,
                                  "--do-not-confirm", "burn-efuse",
                                  "SPI_BOOT_CRYPT_CNT", "7",
                                  "SECURE_BOOT_EN", "1"],
            ctx.dry, why="enable flash encryption + secure boot v2")
    if ctx.dry:
        return
    ctx.refresh()
    if not (_ef(ctx.summary, "SECURE_BOOT_EN") and
            _ef(ctx.summary, "SPI_BOOT_CRYPT_CNT") == 7):
        die("read-back failed: SPI_BOOT_CRYPT_CNT/SECURE_BOOT_EN not set as "
            "expected - inspect the chip manually")
    _stamp(ctx.manifest, "enable_lockdown")
    ctx.save()
    input("\n  Reset the badge (leave download mode) and press Enter to "
          "verify the boot over serial... ")
    badge = BadgeSerial(ctx.args.port, ctx.args.pin).connect()
    try:
        badge.authenticate()
        profile = _parse_profile(badge.command("VERSION", timeout=8))
        if not profile:
            die("badge did not report a Profile line after lockdown - boot "
                "verification FAILED (badge may still be recoverable via reflash)")
        problems = [f"{k}={profile.get(k)}" for k, want in
                    (("flash_enc", "1"), ("secure_boot", "1"),
                     ("debug", "0"), ("secure_serial", "1"))
                    if profile.get(k) != want]
        if problems:
            die("boot verification FAILED: " + ", ".join(problems) +
                " - fix the image and reflash before any further lockdown")
        _stamp(ctx.manifest, "verified_boot", profile=profile)
        info("OK: badge booted encrypted + signed (flash_enc=1 secure_boot=1)")
    finally:
        badge.close()


def _step_harden(ctx):
    ctx.gate.confirm(
        title="HARDEN DOWNLOAD MODE + JTAG (pad + USB)",
        consequences=["JTAG is disabled permanently (pad AND USB bridge).",
                      "Download-mode cache/manual-encrypt access is disabled.",
                      "eFuse bits can NEVER be un-burned."],
        phrase="BURN HARDENING EFUSES",
        serial=ctx.manifest.get("esp_mac"))
    burn = []
    for name in HARDEN_EFUSES:
        burn += [name, "1"]
    run_cmd(list(ctx.espefuse) + ["--chip", CHIP, "--port", ctx.port,
                                  "--do-not-confirm", "burn-efuse"] + burn,
            ctx.dry, why="harden: caches / JTAG (pad + USB) / direct boot")
    if not ctx.dry:
        ctx.refresh()
        missing = [n for n in HARDEN_EFUSES if not _ef(ctx.summary, n)]
        if missing:
            die(f"read-back failed: {', '.join(missing)} still 0 after burning")
        _stamp(ctx.manifest, "harden")


LOCK_STEPS = [
    # (key, description, run, done(ctx) -> bool)
    ("gen_fe_key", "Generate host flash-encryption key",
     _step_gen_fe,
     lambda c: os.path.exists(c.path("fe_key.bin"))),
    ("gen_sb_key", "Generate SB signing key + digest",
     _step_gen_sb,
     lambda c: os.path.exists(c.path("sb_key.pem"))
     and os.path.exists(c.path("sb_digest.bin"))),
    ("burn_fe_key", "Burn FE key into a free key block",
     _step_burn_fe,
     lambda c: _purpose_block(c.summary, "XTS_AES_128_KEY") is not None),
    ("burn_sb_digest", "Burn SB public-key digest",
     _step_burn_sb,
     lambda c: _purpose_block(c.summary, "SECURE_BOOT_DIGEST0") is not None),
    ("flash_release", "Sign + encrypt + flash release images (--dir)",
     _step_flash_release,
     lambda c: bool(c.manifest["stages"].get("flash_release", {}).get("done"))),
    ("enable_lockdown", "Burn SPI_BOOT_CRYPT_CNT=7 + SECURE_BOOT_EN, verify boot",
     _step_enable,
     lambda c: bool(_ef(c.summary, "SECURE_BOOT_EN"))
     and _ef(c.summary, "SPI_BOOT_CRYPT_CNT") == 7),
    ("harden", "Harden: caches / JTAG (pad + USB) / direct boot",
     _step_harden,
     lambda c: all(_ef(c.summary, n) for n in HARDEN_EFUSES)),
]


def _lock_status(ctx):
    """Return (list of (key, desc, done), first_open_index)."""
    rows, first_open = [], None
    for idx, (key, desc, _run, done) in enumerate(LOCK_STEPS):
        d = bool(done(ctx))
        rows.append((key, desc, d))
        if not d and first_open is None:
            first_open = idx
    return rows, first_open


def _check_consistency(rows, first_open):
    """A done step AFTER the first open one means the chip state contradicts
    the expected sequence - never auto-continue then."""
    if first_open is None:
        return
    late = [key for (key, _desc, done) in rows[first_open + 1:] if done]
    if late:
        die("inconsistent chip/manifest state: step(s) "
            f"{', '.join(late)} are already done while an earlier step is "
            "not. Refusing to continue automatically - inspect "
            "manifest.json and `espefuse summary` manually")


def cmd_lock_esp(args):
    dry = args.dry_run

    banner([
        "  ESP32-S3 PRODUCTION LOCKDOWN (Flash Encryption + Secure Boot v2)",
        "  Strict state machine: `--continue` runs exactly the next open step.",
        "  Badge must be in ROM download mode (AUTH <pin> then BOOTLOADER).",
        "  Release images come from `pio run -e cdc_badge_release` (--dir).",
    ])

    if dry:
        print("\n  Planned sequence (dry-run, no hardware touched):\n")
        for idx, (key, desc, _run, _done) in enumerate(LOCK_STEPS):
            print(f"  [{idx}] {key:16s} {desc}")
        print(YEL + "\n  [dry-run] connect a badge and use `lock-esp` for live "
              "status, `lock-esp --continue` to execute the next step." + RST)
        return 0

    _ensure_gitignore_secrets()
    ctx = Ctx(args, dry)
    ctx.load()
    rows, first_open = _lock_status(ctx)
    _check_consistency(rows, first_open)

    print(f"\n  Badge ESP MAC: {ctx.manifest['esp_mac']}   "
          f"state dir: {os.path.relpath(ctx.chip_dir, REPO_ROOT)}\n")
    for idx, (key, desc, done) in enumerate(rows):
        mark = "done" if done else ("NEXT" if idx == first_open else "open")
        print(f"  [{idx}] {mark:4s}  {key:16s} {desc}")

    if first_open is None:
        info("\nall lock-esp steps are complete on this badge")
        ctx.save()
        return 0

    if not args.cont:
        info(f"\nrun with --continue to execute step [{first_open}] "
             f"({LOCK_STEPS[first_open][0]})")
        ctx.save()
        return 0

    key, desc, run, done = LOCK_STEPS[first_open]
    print(f"\n  Executing step [{first_open}] {key}: {desc}\n")
    run(ctx)
    if not done(ctx):
        die(f"step {key} did not reach its expected end state - inspect manually")
    _stamp(ctx.manifest, key)
    ctx.save()
    info(f"step {key} complete. Re-run `lock-esp --continue` for the next step.")
    return 0


# --- subcommand: reflash --------------------------------------------------------

def cmd_reflash(args):
    dry = args.dry_run

    banner([
        "  REFLASH A LOCKED BADGE (sign -> verify -> encrypt -> one write-flash)",
        "  Badge must be in download mode (AUTH <pin> then BOOTLOADER).",
        "  Fails if DIS_DOWNLOAD_MODE was burned (permanent lock).",
    ])

    if dry:
        espsecure = _resolve_tool("espsecure", verify=False)
        esptool_ = _resolve_tool("esptool", verify=False)
        images = _prepare_images(args.dir, "tools/secrets/esp-<mac>/sb_key.pem",
                                 "tools/secrets/esp-<mac>/fe_key.bin",
                                 espsecure, dry)
        _flash_images(images, esptool_, args.port or "PORT", dry)
        return 0

    ctx = Ctx(args, dry)
    ctx.load()
    sb = ctx.path("sb_key.pem")
    fe = ctx.path("fe_key.bin")
    _check_key(ctx.manifest, "sb_key", sb)
    _check_key(ctx.manifest, "fe_key", fe)
    images = _prepare_images(args.dir, sb, fe, ctx.espsecure, dry)
    _flash_images(images, ctx.esptool, ctx.port, dry)
    _stamp(ctx.manifest, "reflash",
           images={hex(a): _sha256(p) for a, p in images.items()})
    ctx.save()
    return 0


# --- subcommand: disable-download ------------------------------------------------

def cmd_disable_download(args):
    dry = args.dry_run
    gate = DangerGate(args, dry)

    if not dry:
        ctx = Ctx(args, dry)
        ctx.load()
        rows, first_open = _lock_status(ctx)
        if first_open is not None:
            die(f"refused: lock-esp step '{rows[first_open][0]}' is still open. "
                "disable-download is the LAST action")
        if not ctx.manifest["stages"].get("verified_boot", {}).get("done"):
            die("refused: no verified boot of the final signed/encrypted "
                "firmware is recorded in the manifest (lock-esp step "
                "enable_lockdown includes it)")
        port = ctx.port
        espefuse = ctx.espefuse
        serial_id = ctx.manifest.get("esp_mac")
    else:
        port = args.port or "PORT"
        espefuse = _resolve_tool("espefuse", verify=False)
        serial_id = None

    if args.mode == "security":
        gate.confirm(
            title="ENABLE SECURITY DOWNLOAD MODE",
            consequences=[
                "ROM download stays, but only encrypted writes are allowed.",
                "espefuse can no longer burn any further eFuses after this.",
                "Serial reflash of pre-encrypted images (reflash cmd) still works."],
            phrase="ENABLE SECURITY DOWNLOAD",
            serial=serial_id)
        efuse = "ENABLE_SECURITY_DOWNLOAD"
    else:  # permanent
        gate.confirm(
            title="PERMANENTLY DISABLE ALL FLASHING (DIS_DOWNLOAD_MODE)",
            consequences=[
                "The ROM download mode is destroyed forever.",
                "This firmware has NO OTA path, so the badge can NEVER be",
                "reflashed, recovered, or updated again. It is frozen for good."],
            phrase="DISABLE DOWNLOAD MODE FOREVER",
            extra_word="PERMANENTLY-DISABLE-ALL-FLASHING",
            serial=serial_id)
        efuse = "DIS_DOWNLOAD_MODE"

    cmd = list(espefuse) + ["--chip", CHIP, "--port", port, "--do-not-confirm"]
    if dry:
        cmd += ["--virt"]
    cmd += ["burn-efuse", efuse, "1"]
    run_cmd(cmd, dry, why=f"burn {efuse}")
    if not dry:
        _stamp(ctx.manifest, f"disable_download_{args.mode}")
        ctx.save()
    return 0


# --- argparse --------------------------------------------------------------------

def _add_common(sp):
    sp.add_argument("--port", help="serial port (auto-detected if omitted)")
    sp.add_argument("--pin", help="badge PIN for AUTH (if secure serial is on)")
    sp.add_argument("--dry-run", action="store_true",
                    help="show exactly what would happen; change nothing")
    sp.add_argument("--i-understand-this-is-irreversible", action="store_true",
                    help="required real-run acknowledgement for destructive ops")


def build_parser():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    rk = sub.add_parser("rotate-key", help="rotate TROPIC01 SH0 sync key")
    _add_common(rk)
    rk.add_argument("--new-slot", type=int, default=1, choices=[1, 2, 3],
                    help="free pairing slot for the new key (default 1)")
    rk.add_argument("--verify", action="store_true",
                    help="verify the badge authenticates via the NEW slot")
    rk.add_argument("--invalidate-old-slot", type=int, choices=[0, 1, 2, 3],
                    help="PERMANENTLY invalidate this old slot (requires --verify "
                    "to have succeeded)")
    rk.set_defaults(func=cmd_rotate_key)

    le = sub.add_parser("lock-esp", help="flash encryption + secure boot v2 "
                        "(strict state machine)")
    _add_common(le)
    le.add_argument("--continue", dest="cont", action="store_true",
                    help="execute exactly the next open step")
    le.add_argument("--dir", help="release build dir (needed for the "
                    "flash_release step; from `pio run -e cdc_badge_release`)")
    le.set_defaults(func=cmd_lock_esp)

    rf = sub.add_parser("reflash", help="reflash a locked badge over serial")
    _add_common(rf)
    rf.add_argument("--dir", required=True, help="directory with the 3 .bin files")
    rf.set_defaults(func=cmd_reflash)

    dd = sub.add_parser("disable-download", help="final download-mode lock")
    _add_common(dd)
    dd.add_argument("--mode", choices=["security", "permanent"], default="security",
                    help="security = updatable (default); permanent = frozen forever")
    dd.set_defaults(func=cmd_disable_download)
    return p


def main():
    parser = build_parser()
    # --help stays readable while disarmed; any real invocation is latched.
    if len(sys.argv) == 1 or sys.argv[1] in ("-h", "--help"):
        parser.print_help()
        return 0
    _require_armed()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
