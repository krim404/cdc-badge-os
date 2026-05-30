#!/usr/bin/env python3
"""
CDC Badge OS - Firmware Flash Tool

Flashes pre-built firmware binaries (from GitHub CI/Release) to the CDC Badge.
Pure Python - requires only esptool and requests (pip install -r requirements.txt).

Usage:
    # Flash from a local directory containing the 3 bin files
    python tools/flash_firmware.py --dir ./artifacts/

    # Download and flash the latest GitHub release
    python tools/flash_firmware.py --release latest

    # Download and flash a specific release tag
    python tools/flash_firmware.py --release v1.0.0

    # Specify serial port manually
    python tools/flash_firmware.py --dir ./artifacts/ --port /dev/cu.usbmodem1101
"""

import argparse
import glob
import os
import sys
import tempfile

CHIP = "esp32s3"
BAUD = 460800
FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "16MB"

# Flash layout (address -> filename pattern). Offsets must match partitions.csv
# (app0 lives at 0x50000, not the ESP-IDF default 0x10000).
FLASH_MAP = {
    0x0:      "bootloader",
    0x8000:   "partitions",
    0x50000:  "firmware",
}

GITHUB_REPO = "krim404/cdc-badge-os"
GITHUB_API = f"https://api.github.com/repos/{GITHUB_REPO}"


def find_port():
    """Auto-detect USB serial port."""
    patterns = ["/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*", "COM*"]
    for pattern in patterns:
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def find_bin_file(directory, keyword):
    """Find a bin file in directory matching the keyword (bootloader/partitions/firmware)."""
    candidates = []
    for f in os.listdir(directory):
        if not f.endswith(".bin"):
            continue
        lower = f.lower()
        if keyword in lower:
            candidates.append(os.path.join(directory, f))

    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        # Prefer exact match patterns from CI naming
        for c in candidates:
            if f"cdc-badge-{keyword}" in os.path.basename(c).lower():
                return c
        return candidates[0]
    return None


def resolve_binaries(directory):
    """Resolve all 3 required binaries from a directory."""
    binaries = {}
    for addr, keyword in FLASH_MAP.items():
        path = find_bin_file(directory, keyword)
        if not path:
            print(f"ERROR: No {keyword}.bin found in {directory}")
            print(f"  Expected file matching '*{keyword}*.bin'")
            sys.exit(1)
        binaries[addr] = path
    return binaries


def download_release(tag):
    """Download release binaries from GitHub."""
    try:
        import requests
    except ImportError:
        print("ERROR: 'requests' package required for downloading releases")
        print("  pip install requests")
        sys.exit(1)

    if tag == "latest":
        url = f"{GITHUB_API}/releases/latest"
    else:
        url = f"{GITHUB_API}/releases/tags/{tag}"

    print(f"Fetching release info from {url}")
    resp = requests.get(url, timeout=15)
    if resp.status_code == 404:
        print(f"ERROR: Release '{tag}' not found")
        if tag != "latest":
            print("  Available releases: check https://github.com/"
                  f"{GITHUB_REPO}/releases")
        sys.exit(1)
    resp.raise_for_status()

    release = resp.json()
    print(f"Release: {release['tag_name']} - {release['name']}")

    tmpdir = tempfile.mkdtemp(prefix="cdc-badge-")
    assets = release.get("assets", [])
    if not assets:
        print("ERROR: Release has no binary assets attached")
        sys.exit(1)

    downloaded = []
    for asset in assets:
        name = asset["name"]
        if not name.endswith(".bin"):
            continue
        dl_url = asset["browser_download_url"]
        dest = os.path.join(tmpdir, name)
        print(f"  Downloading {name} ({asset['size'] // 1024} KB)...")
        r = requests.get(dl_url, timeout=60)
        r.raise_for_status()
        with open(dest, "wb") as f:
            f.write(r.content)
        downloaded.append(name)

    if not downloaded:
        print("ERROR: No .bin files found in release assets")
        sys.exit(1)

    print(f"  Downloaded {len(downloaded)} file(s) to {tmpdir}")
    return tmpdir


def flash(port, binaries, erase_nvs=False):
    """Flash binaries using esptool as Python library."""
    try:
        import esptool
    except ImportError:
        print("ERROR: 'esptool' package required")
        print("  pip install esptool")
        sys.exit(1)

    print()
    print("=" * 60)
    print("CDC Badge OS - Flash Firmware")
    print("=" * 60)
    print(f"  Port:  {port}")
    print(f"  Chip:  {CHIP}")
    print(f"  Baud:  {BAUD}")
    print(f"  Flash: {FLASH_SIZE} {FLASH_MODE} @ {FLASH_FREQ}")
    print()
    for addr, path in sorted(binaries.items()):
        size_kb = os.path.getsize(path) / 1024
        print(f"  0x{addr:06X}  {os.path.basename(path):45s} ({size_kb:.1f} KB)")
    print()

    # Build esptool command arguments
    cmd = [
        "--chip", CHIP,
        "--port", port,
        "--baud", str(BAUD),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
    ]

    for addr, path in sorted(binaries.items()):
        cmd.extend([hex(addr), path])

    print("Flashing...")
    try:
        esptool.main(cmd)
    except SystemExit as e:
        if e.code != 0:
            print(f"\nERROR: esptool exited with code {e.code}")
            print("Tip: Hold BOOT button while pressing RESET to enter download mode")
            sys.exit(1)

    if erase_nvs:
        print("\nErasing NVS partition (0x9000, 20KB)...")
        erase_cmd = [
            "--chip", CHIP,
            "--port", port,
            "--baud", str(BAUD),
            "erase_region", "0x9000", "0x5000",
        ]
        try:
            esptool.main(erase_cmd)
        except SystemExit:
            print("WARNING: NVS erase failed (non-critical)")

    print()
    print("=" * 60)
    print("Flash complete! Device will reset automatically.")
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(
        description="CDC Badge OS - Flash pre-built firmware from CI/Release",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --dir ./artifacts/              Flash from local directory
  %(prog)s --release latest                Download & flash latest release
  %(prog)s --release v1.0.0               Download & flash specific version
  %(prog)s --dir ./artifacts/ --erase-nvs  Flash and wipe settings
        """,
    )

    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--dir", metavar="PATH",
                        help="Directory containing bootloader/partitions/firmware .bin files")
    source.add_argument("--release", metavar="TAG",
                        help="GitHub release tag to download (or 'latest')")

    parser.add_argument("--port", metavar="PORT",
                        help="Serial port (auto-detected if omitted)")
    parser.add_argument("--erase-nvs", action="store_true",
                        help="Erase NVS partition after flashing (resets all settings)")

    args = parser.parse_args()

    # Resolve source directory
    if args.dir:
        src_dir = args.dir
        if not os.path.isdir(src_dir):
            print(f"ERROR: Directory not found: {src_dir}")
            sys.exit(1)
    elif args.release:
        src_dir = download_release(args.release)

    # Find binaries
    binaries = resolve_binaries(src_dir)

    # Resolve port
    port = args.port or find_port()
    if not port:
        print("ERROR: No USB serial port found")
        print("  Connect the badge via USB and try again")
        print("  Or specify --port manually")
        sys.exit(1)

    # Flash
    flash(port, binaries, erase_nvs=args.erase_nvs)


if __name__ == "__main__":
    main()
