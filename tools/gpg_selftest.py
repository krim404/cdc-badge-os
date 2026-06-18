#!/usr/bin/env python3
from __future__ import annotations
"""
Automated on-device GPG self-test driver for CDC Badge OS.

Talks to the regular (already-flashed) firmware over the USB CDC serial port,
authenticates, and runs the in-firmware software-RSA self-test
(`GPG RSA_SELFTEST`), which exercises key generation, blob round-trip, signing,
verification and decryption on the device. No special test firmware is needed.

Usage:
    python3 tools/gpg_selftest.py                     # auto-detect port, RSA-2048
    python3 tools/gpg_selftest.py --bits 2048 3072    # several sizes
    python3 tools/gpg_selftest.py --bits all          # 2048, 3072, 4096 (4096 is slow)
    python3 tools/gpg_selftest.py --port /dev/cu.usbmodemXXXX --pin 0000

Exit code 0 if every requested self-test reports OK, 1 otherwise.

Requirements:
    pip install pyserial
"""

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Error: 'pyserial' package required. Install with: pip install pyserial")
    sys.exit(2)

BAUD = 115200
ALL_SIZES = [2048, 3072, 4096]


def autodetect_port() -> str | None:
    """Return the first plausible CDC Badge serial port, or None."""
    candidates = []
    for p in list_ports.comports():
        haystack = " ".join(filter(None, [p.device, p.description, p.manufacturer,
                                           getattr(p, "product", None)])).lower()
        if "badge" in haystack or "usbmodem" in (p.device or "").lower() or "cdc" in haystack:
            candidates.append(p.device)
    return candidates[0] if candidates else None


def drain(ser: serial.Serial, seconds: float = 0.3) -> str:
    """Read everything available within a short window."""
    deadline = time.time() + seconds
    out = []
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            out.append(chunk.decode("utf-8", "replace"))
            deadline = time.time() + seconds
    return "".join(out)


def send(ser: serial.Serial, line: str) -> None:
    ser.write((line + "\r\n").encode())
    ser.flush()


def run_selftest(ser: serial.Serial, bits: int, timeout: float) -> bool:
    """Run one RSA self-test and parse the OK/FAIL marker."""
    send(ser, f"GPG RSA_SELFTEST {bits}")
    marker = f"RSA_SELFTEST {bits}:"
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
            if marker in buf:
                line = buf[buf.index(marker):]
                ok = "OK" in line.split("\n", 1)[0]
                status = "OK" if ok else "FAIL"
                print(f"  RSA-{bits}: {status}")
                return ok
    print(f"  RSA-{bits}: TIMEOUT (no result within {timeout:.0f}s)")
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="On-device GPG RSA self-test driver")
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--pin", default="0000", help="Badge PIN for AUTH (default 0000)")
    ap.add_argument("--bits", nargs="+", default=["2048"],
                    help="Modulus sizes to test, or 'all' (default 2048)")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="Per-test timeout in seconds (RSA-4096 keygen is slow)")
    args = ap.parse_args()

    if len(args.bits) == 1 and args.bits[0].lower() == "all":
        sizes = ALL_SIZES
    else:
        try:
            sizes = [int(b) for b in args.bits]
        except ValueError:
            print("--bits must be integers (2048/3072/4096) or 'all'")
            return 2
    for b in sizes:
        if b not in ALL_SIZES:
            print(f"Unsupported size {b}; choose from {ALL_SIZES}")
            return 2

    port = args.port or autodetect_port()
    if not port:
        print("No serial port found. Specify with --port.")
        return 2

    print(f"Connecting to {port} @ {BAUD}...")
    try:
        ser = serial.Serial(port, BAUD, timeout=0.2)
    except serial.SerialException as e:
        print(f"Failed to open {port}: {e}")
        return 2

    with ser:
        time.sleep(0.5)
        drain(ser, 0.4)
        send(ser, f"AUTH {args.pin}")
        auth_reply = drain(ser, 0.6)
        if "ERROR" in auth_reply and "authenticated" in auth_reply.lower():
            print("AUTH failed; check --pin.")
            return 1

        print(f"Running RSA self-tests: {sizes}")
        all_ok = True
        for b in sizes:
            if not run_selftest(ser, b, args.timeout):
                all_ok = False

    print("RESULT:", "PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
