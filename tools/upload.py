#!/usr/bin/env python3
"""
CDC Badge OS - unified upload tool.

One tool, several modes selected by the parameters you pass; all share the
USB-CDC serial port and the optional AUTH PIN (FEATURE_SECURE_SERIAL).

Modes:
    Plugin   --wasm <f> --meta <f> [--lang <f>]   install/update a plugin
             --list | --info <id> | --delete <id> | --start <id> | --stop
    Overlay  --lang-overlay <lang_<code>.json>     write a UI language file to
                                                   /plugins/i18n/ + reload it
    File     --put <local> [--dir <vfat-dir>] [--name <n>]
                                                   stream a file into the
                                                   plugins partition via
                                                   VFAT RECEIVE

Examples:
    python tools/upload.py --wasm hello.wasm --meta hello.meta.json --pin 0000
    python tools/upload.py --lang-overlay assets/i18n/lang_de.json --pin 0000
    python tools/upload.py --put notes.txt --dir data --pin 0000
    python tools/upload.py --list --pin 0000

Required: pyserial (`pip install pyserial`).
"""

import argparse
import binascii
import glob
import json
import sys
import time
from pathlib import Path

DEFAULT_CHUNK = 256
SERIAL_BAUD   = 115200
ACK_TIMEOUT_S = 5
END_TIMEOUT_S = 15
MAX_RETRIES   = 5


def detect_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*", "COM*"):
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def require_port(args):
    port = args.port or detect_port()
    if not port:
        sys.exit("ERROR: no USB serial port detected, use --port")
    return port


def open_port(port):
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
    # Bare LF terminator: with CRLF the badge executes on \r and the trailing
    # \n is swallowed as the first payload byte of a streamed upload.
    p.write((line + "\n").encode("utf-8"))
    p.flush()


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def authenticate(p, pin):
    """AUTH <pin>; required when FEATURE_SECURE_SERIAL is enabled."""
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
    """Read lines until one starts with any prefix, skipping echoes/prompts."""
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


# --- File receive protocol (raw binary stream, whole-file CRC) -------------
# One mechanism for every file the badge accepts: a command arms a byte
# interceptor and replies READY, then `size` raw bytes follow whose CRC32 is
# verified. Plugin install uses "PLUGIN UPLOAD*", everything else "VFAT RECEIVE".

_KIND_CMD = {"wasm": "UPLOAD", "aot": "UPLOAD_AOT", "meta": "UPLOAD_META", "lang": "UPLOAD_LANG"}


def stream_payload(p, command, data, progress=None):
    """Send `command` (arms the receiver), then stream `data` and wait for OK.

    `command` is the subcommand without the trailing "<size> <crc>", e.g.
    "PLUGIN UPLOAD hello" or "VFAT RECEIVE data/notes.txt".
    """
    total = len(data)
    send_line(p, f"{command} {total} {crc32(data):08x}")
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


def upload_plugin_file(p, plugin_id, path, kind, progress=None):
    return stream_payload(p, f"PLUGIN {_KIND_CMD[kind]} {plugin_id}",
                          Path(path).read_bytes(), progress)


def vfat_receive(p, relpath, data, progress=None):
    """Stream a file into the plugins partition via the VFAT serial shell."""
    return stream_payload(p, f"VFAT RECEIVE {relpath}", data, progress)


def safe_abort(p):
    try:
        send_line(p, "PLUGIN ABORT")
        readline(p, timeout=1)
    except Exception:
        pass


# --- Commands --------------------------------------------------------------

def cmd_plugin(args):
    p = open_port(require_port(args))
    authenticate(p, args.pin)
    plugin_id = args.id or json.loads(Path(args.meta).read_text())["id"]
    try:
        print(f"Uploading meta -> {args.meta}")
        upload_plugin_file(p, plugin_id, args.meta, "meta",
                           lambda f: print(f"  meta {f*100:5.1f} %", end="\r"))
        print()
        kind = "aot" if args.wasm.lower().endswith(".aot") else "wasm"
        print(f"Uploading {kind} -> {args.wasm}")
        upload_plugin_file(p, plugin_id, args.wasm, kind,
                           lambda f: print(f"  {kind} {f*100:5.1f} %", end="\r"))
        print()
        if args.lang:
            print(f"Uploading lang -> {args.lang}")
            upload_plugin_file(p, plugin_id, args.lang, "lang",
                               lambda f: print(f"  lang {f*100:5.1f} %", end="\r"))
            print()
        print(f"Installed {plugin_id}.")
    except Exception:
        safe_abort(p)
        raise


def cmd_lang(args):
    path = Path(args.lang_overlay)
    if not path.is_file():
        sys.exit(f"ERROR: file not found: {path}")
    name = path.name
    if not (name.startswith("lang_") and name.endswith(".json")):
        sys.exit(f"ERROR: expected a lang_<code>.json file, got: {name}")
    p = open_port(require_port(args))
    authenticate(p, args.pin)
    send_line(p, "VFAT CD /")
    readline(p, timeout=2)
    send_line(p, "VFAT MKDIR i18n")
    readline(p, timeout=2)
    print(f"Uploading overlay -> /plugins/i18n/{name} ({path})")
    vfat_receive(p, f"i18n/{name}", path.read_bytes(),
                 lambda f: print(f"  {f * 100:5.1f} %", end="\r"))
    print()
    send_line(p, "LANG RELOAD")
    print(f"Reload: {wait_for(p, ['OK', 'ERR'], timeout=5) or '(no response)'}")


def cmd_put(args):
    """Stream a file into the plugins partition via VFAT RECEIVE (binary)."""
    path = Path(args.put)
    if not path.is_file():
        sys.exit(f"ERROR: file not found: {path}")
    name = args.name or path.name
    relpath = f"{args.dir}/{name}" if args.dir else name

    p = open_port(require_port(args))
    authenticate(p, args.pin)
    send_line(p, "VFAT CD /")
    readline(p, timeout=2)
    if args.dir:
        send_line(p, f"VFAT MKDIR {args.dir}")
        readline(p, timeout=2)
    print(f"Uploading file -> /{relpath} ({path})")
    vfat_receive(p, relpath, path.read_bytes(),
                 lambda f: print(f"  {f * 100:5.1f} %", end="\r"))
    print()


def _simple(args, command, timeout=5):
    p = open_port(require_port(args))
    authenticate(p, args.pin)
    send_line(p, command)
    return p


def cmd_list(args):
    p = _simple(args, "PLUGIN LIST")
    out, end_time = [], time.time() + 5
    while time.time() < end_time:
        line = readline(p, timeout=1)
        if not line:
            continue
        out.append(line)
        if line.strip().endswith("]"):
            break
    print("\n".join(out))


def cmd_info(args):
    p = _simple(args, f"PLUGIN INFO {args.info}")
    deadline = time.time() + 5
    while time.time() < deadline:
        line = readline(p, timeout=1)
        if not line:
            continue
        print(line)
        if line.startswith("ERR") or line.startswith("prereqs:"):
            break


def cmd_delete(args):
    print(readline(_simple(args, f"PLUGIN DELETE {args.delete}")))


def cmd_start(args):
    print(readline(_simple(args, f"PLUGIN START {args.start}"), timeout=10))


def cmd_stop(args):
    print(readline(_simple(args, "PLUGIN STOP")))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--pin",  help="Badge PIN for AUTH (FEATURE_SECURE_SERIAL)")
    ap.add_argument("--id",   help="Plugin id (overrides meta.json#id)")
    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument("--wasm", help="Plugin .wasm/.aot (with --meta)")
    grp.add_argument("--lang-overlay", dest="lang_overlay", metavar="LANG_CODE_JSON",
                     help="Upload a UI language file (lang_<code>.json) to /plugins/i18n/")
    grp.add_argument("--put", metavar="LOCAL", help="Upload a file via VFAT RECEIVE")
    grp.add_argument("--list", action="store_true")
    grp.add_argument("--info", metavar="ID")
    grp.add_argument("--delete", metavar="ID")
    grp.add_argument("--start",  metavar="ID")
    grp.add_argument("--stop",   action="store_true")
    ap.add_argument("--meta", help="Plugin meta.json (required with --wasm)")
    ap.add_argument("--lang", help="Plugin <id>.lang.json (optional, with --wasm)")
    ap.add_argument("--dir",  help="Target directory for --put (default: root)")
    ap.add_argument("--name", help="Stored file name for --put (default: source name)")
    args = ap.parse_args()

    if args.wasm:
        if not args.meta:
            ap.error("--meta is required with --wasm")
        cmd_plugin(args)
    elif args.lang_overlay: cmd_lang(args)
    elif args.put:          cmd_put(args)
    elif args.list:         cmd_list(args)
    elif args.info:         cmd_info(args)
    elif args.delete:       cmd_delete(args)
    elif args.start:        cmd_start(args)
    elif args.stop:         cmd_stop(args)


if __name__ == "__main__":
    main()
