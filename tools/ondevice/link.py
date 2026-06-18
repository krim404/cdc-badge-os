"""Serial link and test plumbing for the on-device harness.

Mirrors the serial conventions of the other tools (tools/upload.py,
tools/backup.py, tools/2fa.py): pyserial @ 115200, bare-LF line terminator,
the AUTH PIN gate, and the wait_for/collect response patterns. Adds a small
class wrapper plus operator-prompt primitives for the semi-automatic catalog.

Required: pyserial (`pip install pyserial`).
"""

import glob
import re
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

SERIAL_BAUD = 115200
ACK_TIMEOUT_S = 5
END_TIMEOUT_S = 15


class SkipTest(Exception):
    """Raised by a test that cannot run (missing prerequisite, feature off)."""


class CheckError(AssertionError):
    """Raised when an assertion inside a test fails."""


def detect_port() -> Optional[str]:
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*", "COM*"):
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def require(condition: bool, message: str) -> None:
    """Assert `condition`, raising CheckError(message) on failure."""
    if not condition:
        raise CheckError(message)


def joined(lines: List[str]) -> str:
    return "\n".join(lines)


def find_line(lines: List[str], needle: str) -> Optional[str]:
    """Return the first line containing `needle`, or None."""
    for line in lines:
        if needle in line:
            return line
    return None


def match_line(lines: List[str], pattern: str) -> Optional["re.Match"]:
    """Return the first regex match against any line, or None."""
    rx = re.compile(pattern)
    for line in lines:
        m = rx.search(line)
        if m:
            return m
    return None


class BadgeSerial:
    """Thin USB-CDC serial client for the badge console."""

    def __init__(self, port: Optional[str] = None, pin: Optional[str] = None):
        self.port = port
        self.pin = pin
        self._ser = None
        self.secure_serial = True  # flipped to False if AUTH is unknown

    def connect(self) -> "BadgeSerial":
        port = self.port or detect_port()
        if not port:
            raise RuntimeError("no USB serial port detected, use --port")
        try:
            import serial
        except ImportError:
            sys.exit("ERROR: install pyserial -> pip install pyserial")
        self._ser = serial.Serial(port, SERIAL_BAUD, timeout=ACK_TIMEOUT_S)
        self.port = port
        time.sleep(0.2)
        self._ser.reset_input_buffer()
        return self

    def close(self) -> None:
        if self._ser:
            self._ser.close()
            self._ser = None

    # --- low-level I/O -----------------------------------------------------

    def _readline(self, timeout: float = ACK_TIMEOUT_S) -> str:
        self._ser.timeout = timeout
        return self._ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")

    def send(self, line: str) -> None:
        # Bare LF: the badge executes on \r and would swallow a trailing \n as
        # the first byte of a streamed upload.
        self._ser.write((line + "\n").encode("utf-8"))
        self._ser.flush()

    def drain(self, quiet: float = 0.3) -> None:
        old = self._ser.timeout
        self._ser.timeout = quiet
        while self._ser.readline():
            pass
        self._ser.timeout = old

    @staticmethod
    def _strip_prompt(s: str) -> str:
        s = s.strip()
        while s.startswith(">"):
            s = s[1:].lstrip()
        return s

    def collect(self, timeout: float = ACK_TIMEOUT_S, quiet: float = 0.4) -> List[str]:
        """Read reply lines until the device goes quiet, prompt-stripped."""
        lines: List[str] = []
        deadline = time.time() + timeout
        self._ser.timeout = quiet
        while time.time() < deadline:
            raw = self._ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
            if not raw:
                if lines:
                    break
                continue
            s = self._strip_prompt(raw)
            if s:
                lines.append(s)
        return lines

    def command(self, cmd: str, timeout: float = ACK_TIMEOUT_S,
                quiet: float = 0.4) -> List[str]:
        """Send one command and return its reply lines."""
        self._ser.reset_input_buffer()
        self.send(cmd)
        return self.collect(timeout=timeout, quiet=quiet)

    def command_until(self, cmd: str, terminator: str,
                      timeout: float = ACK_TIMEOUT_S) -> List[str]:
        """Send a command and read lines until one contains `terminator`.

        Unlike command(), this does not stop at the first blank line, so it can
        capture a multi-line block with embedded blanks such as an ASCII-armored
        OpenPGP key (header line, blank line, base64 body, CRC, END)."""
        self._ser.reset_input_buffer()
        self.send(cmd)
        lines: List[str] = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self._readline(timeout=1)
            if not raw:
                continue
            s = self._strip_prompt(raw)
            if s:
                lines.append(s)
            if terminator in raw:
                break
        return lines

    def wait_for(self, prefixes, timeout: float = ACK_TIMEOUT_S) -> Optional[str]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            resp = self._readline(timeout=1)
            if not resp:
                continue
            s = self._strip_prompt(resp)
            for pref in prefixes:
                if s.startswith(pref):
                    return s
        return None

    def paste(self, cmd: str, body: List[str], terminator: str = "---",
              timeout: float = ACK_TIMEOUT_S) -> List[str]:
        """Run a multiline-paste command (VCARD SET, ATTEST IMPORT)."""
        self._ser.reset_input_buffer()
        self.send(cmd)
        time.sleep(0.2)
        for line in body:
            self.send(line)
        self.send(terminator)
        return self.collect(timeout=timeout)

    # --- auth --------------------------------------------------------------

    def authenticate(self) -> None:
        """AUTH <pin>; tolerates a build without FEATURE_SECURE_SERIAL."""
        if not self.pin:
            return
        self._ser.reset_input_buffer()
        self.send(f"AUTH {self.pin}")
        deadline = time.time() + 5
        while time.time() < deadline:
            resp = self._readline(timeout=1)
            if not resp:
                continue
            u = self._strip_prompt(resp).upper()
            if "AUTHENTICATED" in u or u.startswith("OK"):
                return
            if "UNKNOWN COMMAND" in u:
                self.secure_serial = False
                return
            if "WRONG" in u or "LOCKED" in u or u.startswith("ERROR"):
                raise RuntimeError(f"AUTH failed: {resp}")
        raise RuntimeError("AUTH timed out (no OK response)")

    # --- operator prompts (semi-automatic catalog) -------------------------

    @staticmethod
    def note(message: str) -> None:
        print(f"    | {message}")

    @staticmethod
    def prompt(message: str) -> None:
        """Show an instruction and wait for the operator to press Enter."""
        input(f"    > {message}  [Enter to continue] ")

    @staticmethod
    def confirm(message: str) -> bool:
        """Ask the operator a yes/no question; return True on yes."""
        while True:
            ans = input(f"    ? {message}  [y/n] ").strip().lower()
            if ans in ("y", "yes"):
                return True
            if ans in ("n", "no"):
                return False


@dataclass
class Test:
    """One on-device test."""
    id: str
    feature: str
    fn: Callable[[BadgeSerial], None]
    category: str = "auto"           # auto | semi
    frs: str = "-"
    interaction: str = "serial"      # serial|button|host-ctap2|host-ccid|second-badge|visual
    flags: Tuple[str, ...] = field(default_factory=tuple)  # mutating | slow
