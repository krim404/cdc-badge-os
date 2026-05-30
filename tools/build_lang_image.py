#!/usr/bin/env python3
"""Build a wear-levelled FAT image (plugins partition) seeded with the
per-language i18n overlay files.

The output image holds one `i18n/lang_<code>.json` entry per language file
found in the source directory (e.g. `i18n/lang_de.json`). All remaining
sectors are empty so installed plugins (which live as `<id>.wasm` +
`<id>.meta` in the partition root) can be written next to it without further
preparation.

Run from the repo root:

    python tools/build_lang_image.py \
        --lang-dir assets/i18n \
        --output build/plugins_initial.bin

The script locates wl_fatfsgen.py inside the active ESP-IDF installation.
Defaults match the project's `plugins` partition (2 MB, wear-levelled,
long-name support).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def find_wl_fatfsgen() -> Path:
    """Locate wl_fatfsgen.py from the PlatformIO esp-idf framework."""
    home = Path.home()
    candidates = [
        home / ".platformio" / "packages" / "framework-espidf" / "components"
             / "fatfs" / "wl_fatfsgen.py",
    ]
    env = os.environ.get("IDF_PATH")
    if env:
        candidates.insert(0, Path(env) / "components" / "fatfs" / "wl_fatfsgen.py")

    for c in candidates:
        if c.is_file():
            return c

    raise FileNotFoundError(
        "wl_fatfsgen.py not found. Set IDF_PATH or install PlatformIO "
        "framework-espidf."
    )


def select_python() -> str:
    """Prefer PlatformIO penv interpreter - it has construct and reedsolo
    installed; system Python typically does not."""
    pio = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if pio.is_file():
        return str(pio)
    return sys.executable


def build_image(lang_dir: Path, output: Path, partition_size: int) -> None:
    lang_files = sorted(lang_dir.glob("lang_*.json")) if lang_dir.is_dir() else []
    if not lang_files:
        raise FileNotFoundError(f"no lang_<code>.json files in: {lang_dir}")

    output.parent.mkdir(parents=True, exist_ok=True)
    fatfsgen = find_wl_fatfsgen()

    with tempfile.TemporaryDirectory() as staging:
        i18n_dir = Path(staging) / "i18n"
        i18n_dir.mkdir()
        for f in lang_files:
            shutil.copy(f, i18n_dir / f.name)
        print(f"Seeding i18n with: {', '.join(f.name for f in lang_files)}")

        cmd = [
            select_python(),
            str(fatfsgen),
            "--output_file", str(output),
            "--partition_size", str(partition_size),
            "--sector_size", "4096",
            "--long_name_support",
            "--use_default_datetime",
            staging,
        ]
        print("Running:", " ".join(cmd))
        subprocess.run(cmd, check=True)

    size = output.stat().st_size
    print(f"Wrote {output} ({size} bytes)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lang-dir", type=Path,
                    default=Path("assets/i18n"),
                    help="Directory with lang_<code>.json files "
                         "(default: assets/i18n)")
    ap.add_argument("--output", type=Path,
                    default=Path("build/plugins_initial.bin"),
                    help="Output image path (default: build/plugins_initial.bin)")
    ap.add_argument("--partition-size", type=lambda x: int(x, 0),
                    default=0x200000,
                    help="Partition size in bytes (default: 0x200000 = 2 MiB)")
    args = ap.parse_args()

    try:
        build_image(args.lang_dir, args.output, args.partition_size)
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
