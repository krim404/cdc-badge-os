#!/usr/bin/env python3
"""On-device FIDO2 authenticatorLargeBlobs (0x0C) round-trip test over USB-HID.

Drives the badge's CTAPHID interface with python-fido2: confirms the feature is
advertised in getInfo, then writes a recognisable large-blob array, reads it
back through device NVS, and asserts a byte-exact round-trip. The original array
is restored afterwards. No button press is required: a largeBlob write is gated
by a pinUvAuth token (largeBlobWrite permission), not user presence, so the run
is fully unattended.

Usage:
    python tools/fido2_largeblob.py [--pin 123456]

Required: fido2 (`pip install fido2`).
"""

import argparse
import os
import sys

try:
    from fido2.hid import CtapHidDevice
    from fido2.ctap2 import Ctap2, ClientPin
    from fido2.ctap2.blob import LargeBlobs
    from fido2.ctap2.pin import PinProtocolV1, PinProtocolV2
except ImportError:
    sys.exit("ERROR: install python-fido2 -> pip install fido2")

AAGUID_PREFIX = bytes.fromhex("cdcbad6e")  # CDC Badge AAGUID prefix


def find_badge() -> Ctap2:
    """Return a Ctap2 for the first CDC Badge CTAPHID device, else exit."""
    for dev in CtapHidDevice.list_devices():
        product = (dev.descriptor.product_name or "")
        try:
            ctap = Ctap2(dev)
        except Exception:
            continue
        if "badge" in product.lower() or bytes(ctap.info.aaguid).startswith(AAGUID_PREFIX):
            return ctap
    sys.exit("ERROR: no CDC Badge CTAPHID device found (is the badge plugged in?)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pin", default=os.environ.get("BADGE_PIN", "123456"),
                    help="FIDO2 ClientPIN (default: BADGE_PIN env or 123456)")
    args = ap.parse_args()

    ctap = find_badge()
    info = ctap.info

    if not info.options.get("largeBlobs"):
        print("FAIL: getInfo does not advertise largeBlobs")
        return 1
    max_arr = info.max_large_blob or 0
    print(f"largeBlobs advertised, maxSerializedLargeBlobArray={max_arr}")
    if max_arr < 64:
        print("FAIL: maxSerializedLargeBlobArray too small to be usable")
        return 1

    # A write needs a pinUvAuth token only when a ClientPIN is set.
    pin_set = info.options.get("clientPin") is True
    proto = token = None
    if pin_set:
        proto = PinProtocolV2() if 2 in (info.pin_uv_protocols or []) else PinProtocolV1()
        cp = ClientPin(ctap, proto)
        try:
            token = cp.get_pin_token(args.pin, ClientPin.PERMISSION.LARGE_BLOB_WRITE)
        except Exception as exc:
            print(f"FAIL: could not obtain largeBlobWrite token (pin {args.pin!r}): {exc}")
            return 1
        print("obtained pinUvAuth token with largeBlobWrite permission")
    else:
        print("no ClientPIN set; writing without a token")

    lb = LargeBlobs(ctap, proto, token)

    baseline = lb.read_blob_array()
    print(f"baseline array: {len(baseline)} entry/entries")

    # A structurally valid large-blob array entry: ciphertext(1), nonce(2, 12 B),
    # origSize(3). The values are synthetic; we only assert exact persistence.
    marker = b"CDC-LARGEBLOB-HARNESS-" + os.urandom(8)
    entry = {1: marker, 2: bytes(range(12)), 3: len(marker)}
    try:
        lb.write_blob_array([entry])
    except Exception as exc:
        print(f"FAIL: write_blob_array rejected: {exc}")
        return 1

    read_back = lb.read_blob_array()
    ok = (len(read_back) == 1 and read_back[0].get(1) == marker
          and read_back[0].get(2) == bytes(range(12))
          and read_back[0].get(3) == len(marker))

    # Restore the original array regardless of the result.
    try:
        lb.write_blob_array(list(baseline))
    except Exception as exc:
        print(f"WARNING: could not restore baseline array: {exc}")

    if not ok:
        print(f"FAIL: round-trip mismatch; read back: {read_back}")
        return 1
    print(f"PASS: wrote {len(marker)}-byte blob entry and read it back byte-exact "
          f"through device NVS (digest verified by host)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
