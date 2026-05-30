#!/usr/bin/env python3
"""
\file start_webflasher.py
\brief Local CDC Badge web-flasher.

Serves the contents of `web-flasher/` together with the firmware binaries
from the most recent local PlatformIO build, generates a matching
`manifest.json` (and `manifest_factory.json` when the optional plugins
image is present) and opens the default browser at the resulting URL.

Usage:

    python tools/start_webflasher.py [--port 8000] [--no-browser]
                                     [--build] [--lang]

The Web Serial API used by esp-web-tools accepts plain HTTP on localhost,
so no TLS setup is required.
"""

import argparse
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB_FLASHER = ROOT / "web-flasher"
BUILD_DIR = ROOT / ".pio" / "build" / "cdc_badge_usb"
PLUGINS_INITIAL = ROOT / "build" / "plugins_initial.bin"
LANG_DIR = ROOT / "assets" / "i18n"

# Flash offsets must match partitions.csv.
OFFSET_BOOTLOADER = 0x0
OFFSET_PARTITIONS = 0x8000
OFFSET_FIRMWARE = 0x50000
OFFSET_PLUGINS = 0xDF0000


def find_free_port(preferred: int) -> int:
    """Return ``preferred`` if free, else the next free port above it."""
    for port in range(preferred, preferred + 100):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError(f"No free port in [{preferred}, {preferred + 100})")


def require(path: Path) -> Path:
    if not path.is_file():
        sys.exit(f"Missing build artifact: {path}\n"
                 f"Run `~/.platformio/penv/bin/pio run` first, or pass --build.")
    return path


def head_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=ROOT, text=True
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "dev"


def run_pio_build() -> None:
    pio = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    cmd = [str(pio) if pio.is_file() else "pio", "run"]
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, check=True, cwd=ROOT)


def build_lang_image() -> None:
    PLUGINS_INITIAL.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, str(ROOT / "tools" / "build_lang_image.py"),
        "--lang-dir", str(LANG_DIR),
        "--output", str(PLUGINS_INITIAL),
    ]
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def assemble_webroot(workdir: Path, version: str) -> bool:
    """Populate ``workdir`` with index.html, binaries and manifests.

    \return ``True`` when a factory manifest including the plugins image
    has been produced, ``False`` otherwise.
    """
    shutil.copy(WEB_FLASHER / "index.html", workdir / "index.html")
    shutil.copy(WEB_FLASHER / "badge.jpg", workdir / "badge.jpg")

    shutil.copy(require(BUILD_DIR / "bootloader.bin"), workdir / "bootloader.bin")
    shutil.copy(require(BUILD_DIR / "partitions.bin"), workdir / "partitions.bin")
    shutil.copy(require(BUILD_DIR / "firmware.bin"), workdir / "firmware.bin")

    update_manifest = {
        "name": "CDC Badge OS (Local Build)",
        "version": version,
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [
                {"path": "bootloader.bin", "offset": OFFSET_BOOTLOADER},
                {"path": "partitions.bin", "offset": OFFSET_PARTITIONS},
                {"path": "firmware.bin",   "offset": OFFSET_FIRMWARE},
            ],
        }],
    }
    (workdir / "manifest.json").write_text(json.dumps(update_manifest, indent=2))

    has_factory = PLUGINS_INITIAL.is_file()
    if has_factory:
        shutil.copy(PLUGINS_INITIAL, workdir / "plugins_initial.bin")
        factory_manifest = {
            "name": "CDC Badge OS (Local Factory Setup)",
            "version": version,
            "builds": [{
                "chipFamily": "ESP32-S3",
                "parts": [
                    {"path": "bootloader.bin",      "offset": OFFSET_BOOTLOADER},
                    {"path": "partitions.bin",      "offset": OFFSET_PARTITIONS},
                    {"path": "firmware.bin",        "offset": OFFSET_FIRMWARE},
                    {"path": "plugins_initial.bin", "offset": OFFSET_PLUGINS},
                ],
            }],
        }
        (workdir / "manifest_factory.json").write_text(
            json.dumps(factory_manifest, indent=2)
        )
    return has_factory


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler with terse logging."""

    def log_message(self, fmt, *args):
        sys.stderr.write(f"  {self.address_string()} - {fmt % args}\n")


def main() -> int:
    p = argparse.ArgumentParser(
        description="Local CDC Badge web-flasher",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--port", type=int, default=8000,
                   help="preferred TCP port (default: 8000)")
    p.add_argument("--no-browser", action="store_true",
                   help="do not open the default browser")
    p.add_argument("--build", action="store_true",
                   help="run `pio run` before serving")
    p.add_argument("--lang", action="store_true",
                   help="also build plugins_initial.bin from assets/i18n/lang_*.json")
    args = p.parse_args()

    if args.build:
        run_pio_build()
    if args.lang:
        build_lang_image()

    port = find_free_port(args.port)
    workdir = Path(tempfile.mkdtemp(prefix="cdc-flasher-"))

    try:
        has_factory = assemble_webroot(workdir, version=head_commit())

        url = f"http://127.0.0.1:{port}/"
        os.chdir(workdir)
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), QuietHandler)

        print()
        print("=" * 62)
        print(f"  CDC Badge local web-flasher: {url}")
        print(f"  Webroot: {workdir}")
        print(f"  Update manifest:  manifest.json")
        if has_factory:
            print(f"  Factory manifest: manifest_factory.json")
        else:
            print(f"  Factory manifest: (skipped, no plugins_initial.bin)")
        print(f"  Stop with Ctrl-C")
        print("=" * 62)
        print()

        if not args.no_browser:
            threading.Timer(0.3, lambda: webbrowser.open(url)).start()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping web-flasher...")
            httpd.shutdown()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
