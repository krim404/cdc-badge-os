#!/usr/bin/env python3
"""
CDC Badge OS - 2FA provisioning tool.

Drives the on-device TOTP serial command group (which now covers both TOTP and
HOTP entries) to list, add, delete, and read OATH credentials over the USB CDC
serial shell. The command group keeps its historical "TOTP" keyword for
compatibility; the ADD verb takes an explicit entry type.

On-device serial contract (AUTH-gated):
    TOTP LIST
    TOTP ADD <type:totp|hotp> <name> <secret> [issuer] [digits] [period] [algo] [counter]
    TOTP DEL <index>
    TOTP GET <index>

    secret  : Base32 (RFC 4648), spaces/padding ignored by the firmware
    digits  : 6..8 (default 6)
    period  : TOTP step seconds, 15..300 (default 30); ignored for HOTP
    algo    : sha1 | sha256 | sha512 (default sha1)
    counter : initial HOTP moving factor (default 0); ignored for TOTP

Modes:
    --list                          list all entries
    --add-totp <name> <secret>      add a TOTP entry
    --add-hotp <name> <secret>      add a HOTP entry
    --del <index>                   delete an entry by list index
    --get <index>                   generate one code (advances HOTP counter)

Add options (used with --add-totp / --add-hotp):
    --issuer <text>  --digits <n>  --period <s>  --algo <a>  --counter <n>

Examples:
    python tools/2fa.py --list --pin 123456
    python tools/2fa.py --add-totp GitHub JBSWY3DPEHPK3PXP --issuer GitHub --pin 123456
    python tools/2fa.py --add-hotp Token GEZDGNBVGY3TQOJQ --counter 0 --pin 123456
    python tools/2fa.py --get 0 --pin 123456
    python tools/2fa.py --del 0 --pin 123456

Required: pyserial.
"""

import argparse
import glob
import sys
import time

SERIAL_BAUD = 115200
ACK_TIMEOUT_S = 5


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


def send_line(p, line):
    # Bare LF terminator: the badge executes on \r and would swallow a trailing
    # \n as the first byte of the next command.
    p.write((line + "\n").encode("utf-8"))
    p.flush()


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


def collect(p, timeout=ACK_TIMEOUT_S, quiet=0.4):
    """Read response lines until the device goes quiet."""
    lines = []
    deadline = time.time() + timeout
    p.timeout = quiet
    while time.time() < deadline:
        line = p.readline().decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            if lines:
                break
            continue
        lines.append(line)
    return lines


def run_command(args, command):
    p = open_port(args)
    authenticate(p, args.pin)
    p.reset_input_buffer()
    send_line(p, command)
    for line in collect(p):
        print(line)


def quote(value):
    """Wrap empty tokens so positional placeholders are preserved."""
    return value if value else '""'


def cmd_add(args, entry_type, name, secret):
    parts = [
        "TOTP ADD",
        entry_type,
        name,
        secret,
        quote(args.issuer or ""),
        str(args.digits),
        str(args.period),
        args.algo,
    ]
    if entry_type == "hotp":
        parts.append(str(args.counter))
    run_command(args, " ".join(parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--pin", help="Badge PIN for AUTH (FEATURE_SECURE_SERIAL)")

    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument("--list", action="store_true", help="List all 2FA entries")
    grp.add_argument("--add-totp", nargs=2, metavar=("NAME", "SECRET"),
                     dest="add_totp", help="Add a TOTP entry")
    grp.add_argument("--add-hotp", nargs=2, metavar=("NAME", "SECRET"),
                     dest="add_hotp", help="Add a HOTP entry")
    grp.add_argument("--del", dest="delete", metavar="INDEX", type=int,
                     help="Delete an entry by list index")
    grp.add_argument("--get", metavar="INDEX", type=int,
                     help="Generate one code by list index")

    ap.add_argument("--issuer", default="", help="Issuer text (add modes)")
    ap.add_argument("--digits", type=int, default=6, help="Code digits 6-8 (add modes)")
    ap.add_argument("--period", type=int, default=30, help="TOTP period seconds (add modes)")
    ap.add_argument("--algo", default="sha1", choices=["sha1", "sha256", "sha512"],
                    help="Hash algorithm (add modes)")
    ap.add_argument("--counter", type=int, default=0, help="Initial HOTP counter (add-hotp)")
    args = ap.parse_args()

    if args.list:
        run_command(args, "TOTP LIST")
    elif args.add_totp:
        cmd_add(args, "totp", args.add_totp[0], args.add_totp[1])
    elif args.add_hotp:
        cmd_add(args, "hotp", args.add_hotp[0], args.add_hotp[1])
    elif args.delete is not None:
        run_command(args, f"TOTP DEL {args.delete}")
    elif args.get is not None:
        run_command(args, f"TOTP GET {args.get}")


if __name__ == "__main__":
    main()
