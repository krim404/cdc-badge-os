#!/usr/bin/env python3
"""On-device test runner for CDC Badge OS.

Runs the fully-automatic catalog first (serial-only, unattended); with --semi it
appends the semi-automatic catalog last (operator-assisted). Both are optional:
this is a developer/QA tool, never a CI gate. The release under test is flashed
once; no test or alternate-profile firmware is ever flashed.

Usage:
    python tools/ondevice/run.py --list
    python tools/ondevice/run.py --pin 123456
    python tools/ondevice/run.py --pin 123456 --mutating --slow
    python tools/ondevice/run.py --pin 123456 --semi
    python tools/ondevice/run.py --pin 123456 --only A-PWD,A-2FA

The PIN defaults to $BADGE_PIN or 123456; export BADGE_PIN=0000 for a dev badge.

Required: pyserial (`pip install pyserial`).
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ondevice.catalog_auto import AUTO_TESTS                      # noqa: E402
from ondevice.catalog_semi import SEMI_TESTS                      # noqa: E402
from ondevice.link import BadgeSerial, SkipTest                   # noqa: E402

ALL_TESTS = AUTO_TESTS + SEMI_TESTS


def print_catalog() -> None:
    print(f"{'ID':<11}{'CAT':<6}{'FLAGS':<11}{'INTERACTION':<15}FEATURE  [FRs]")
    for t in ALL_TESTS:
        flags = ",".join(t.flags) or "-"
        print(f"{t.id:<11}{t.category:<6}{flags:<11}{t.interaction:<15}"
              f"{t.feature}  [{t.frs}]")
    print(f"\n{len(AUTO_TESTS)} automatic, {len(SEMI_TESTS)} semi-automatic. "
          "Both optional; semi runs last.")


def select_tests(args) -> list:
    only = {s.strip() for s in args.only.split(",")} if args.only else None
    if only:
        known = {t.id for t in ALL_TESTS}
        for unknown in sorted(only - known):
            print(f"WARNING: unknown test id '{unknown}'", file=sys.stderr)
        # Explicit selection: run exactly these, ignoring flag/semi gating.
        return [t for t in ALL_TESTS if t.id in only]

    chosen = []
    for t in ALL_TESTS:
        if t.category == "semi" and not args.semi:
            continue
        if "mutating" in t.flags and not args.mutating:
            continue
        if "slow" in t.flags and not args.slow:
            continue
        chosen.append(t)
    return chosen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--pin", help="Badge PIN for AUTH (default: BADGE_PIN env or 123456)")
    ap.add_argument("--semi", action="store_true",
                    help="Also run the semi-automatic catalog, after the automatic one")
    ap.add_argument("--mutating", action="store_true",
                    help="Include state-mutating tests (PIN change, GPG, backup)")
    ap.add_argument("--slow", action="store_true",
                    help="Include slow tests (the ~65 s lockout-recovery test)")
    ap.add_argument("--only", metavar="IDS",
                    help="Comma-separated test ids to run exactly (ignores gating)")
    ap.add_argument("--list", action="store_true", help="List the catalog and exit")
    args = ap.parse_args()

    if args.list:
        print_catalog()
        return 0

    tests = select_tests(args)
    if not tests:
        print("No tests selected.")
        return 0

    pin = args.pin or os.environ.get("BADGE_PIN", "123456")

    badge = BadgeSerial(port=args.port, pin=pin)
    try:
        badge.connect()
    except Exception as exc:
        sys.exit(f"ERROR: {exc}")
    print(f"Connected to {badge.port}. Testing the installed release as-is.\n")
    try:
        badge.authenticate()
    except Exception as exc:
        badge.close()
        sys.exit(f"ERROR: {exc}")

    results = []
    try:
        for t in tests:
            print(f"[{t.id}] {t.feature}")
            badge.drain()
            start = time.time()
            try:
                t.fn(badge)
                status, detail = "PASS", ""
            except SkipTest as exc:
                status, detail = "SKIP", str(exc)
            except Exception as exc:  # CheckError or any device/IO failure
                status, detail = "FAIL", str(exc)
            elapsed = time.time() - start
            tail = f" - {detail}" if detail else ""
            print(f"    => {status} ({elapsed:.1f}s){tail}\n")
            results.append((t.id, status, detail))
    finally:
        badge.close()

    npass = sum(1 for _, s, _ in results if s == "PASS")
    nfail = sum(1 for _, s, _ in results if s == "FAIL")
    nskip = sum(1 for _, s, _ in results if s == "SKIP")
    print("=" * 60)
    for tid, status, detail in results:
        tail = f" - {detail}" if detail and status != "PASS" else ""
        print(f"  {status:<5} {tid}{tail}")
    print("=" * 60)
    print(f"{npass} passed, {nfail} failed, {nskip} skipped")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
