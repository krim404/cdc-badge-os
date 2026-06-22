"""Semi-automatic on-device tests.

These run last and need the operator: a button press / user-presence touch, a
host protocol stack (CTAP2, CCID), a second badge, or a look at the display.
Each test drives the serial-checkable parts automatically and prompts the
operator for the rest, then asks for a pass/fail confirmation.

All tests verify the installed release as-is. No firmware is flashed; reaching
a destructive state (duress) is a runtime action, never a reflash.
"""

from .link import BadgeSerial, CheckError, SkipTest, Test, find_line, joined, require


def _pass_if(b: BadgeSerial, question: str) -> None:
    if not b.confirm(question):
        raise CheckError("operator marked this step as failed")


def test_fido2(b: BadgeSerial) -> None:
    ml = b.command("MODULE LIST", timeout=8)
    if ml and "fido2" not in joined(ml).lower():
        b.note("FIDO2 module not visible in MODULE LIST; check the build")
    b.note("Maps to T-HIL01. Plug the badge into a host with a CTAP2 stack.")
    b.prompt("At webauthn.io, register a credential; press Y on the badge when prompted")
    _pass_if(b, "Did registration complete after one on-device Y confirmation?")
    b.prompt("Sign in three times at the same RP, confirming on the badge each time")
    _pass_if(b, "Did each assertion succeed and the signCount increase each time?")


def test_openpgp(b: BadgeSerial) -> None:
    s = b.command("GPG STATUS", timeout=8)
    if find_line(s, "No key configured"):
        raise SkipTest("no GPG key on the badge; generate one before running T-HIL02")
    b.note("Maps to T-HIL02. Needs gpg talking to the badge over CCID.")
    b.prompt("Run `gpg --card-status` on the host")
    _pass_if(b, "Did gpg show the badge card with the expected keys/fingerprints?")
    b.prompt("Sign a test message and decrypt it back via the badge")
    _pass_if(b, "Did sign and decrypt both succeed (PW1/PW3 prompts as expected)?")


def test_ble_transfer(b: BadgeSerial) -> None:
    b.note("Maps to T-HIL03. Needs two badges with BLE.")
    b.prompt("Start a vCard send on badge A and receive on badge B")
    _pass_if(b, "Did the numeric comparison match on both and the vCard arrive intact?")


def test_gpg_ble(b: BadgeSerial) -> None:
    rl = b.command("GPG RECV_LIST", timeout=8)
    if not (rl and find_line(rl, "received keys")):
        b.note("No received cross-sign keys; import a peer key first (RECV_IMPORT)")
    b.note("Maps to T-HIL02/spec 007. Needs a second badge over BLE.")
    b.prompt("Cross-sign a received key and use the on-badge Send Signature action to return it over BLE")
    _pass_if(b, "Did the peer badge receive the cross-signature?")


def test_ble_hid(b: BadgeSerial) -> None:
    b.note("Needs a host paired to the badge BLE HID keyboard.")
    b.prompt("Pair the host and trigger a keystroke send from the badge")
    _pass_if(b, "Did the paired host receive the expected keystrokes?")


def test_usb_otp(b: BadgeSerial) -> None:
    b.note("Needs a host observing the USB HID keyboard (a text field is enough).")
    b.prompt("Trigger a Yubico OTP / HMAC-CR or keyboard send from the badge UI")
    _pass_if(b, "Did the host receive the expected typed output?")


def test_epaper(b: BadgeSerial) -> None:
    b.note("Maps to T-HIL06. Watch the lock-screen clock on the display.")
    b.prompt("Observe the lock-screen clock across at least three minute ticks")
    _pass_if(b, "Did the clock update flash-free (PARTIAL_LIGHT), never a full refresh?")


def test_pin_ui(b: BadgeSerial) -> None:
    b.note("Maps to T-HIL05 (visual half). Use the on-device lock screen.")
    b.prompt("Lock the badge, then enter a wrong PIN until the lockout countdown shows")
    _pass_if(b, "Did a visible 60 s countdown appear and refuse the correct PIN until zero?")
    b.prompt("Let the countdown expire, then enter the correct PIN")
    _pass_if(b, "Did the correct PIN unlock the badge with no reflash needed?")


def test_duress(b: BadgeSerial) -> None:
    b.note("Maps to T-HIL04. THIS WIPES THE BADGE (all ECC + R-Mem + NVS).")
    if not b.confirm("Run the DESTRUCTIVE duress-wipe test on this badge?"):
        raise SkipTest("operator declined the destructive duress test")
    duress = "911911"
    r = b.command(f"PIN DURESS {duress}")
    require(find_line(r, "OK"), f"could not arm duress PIN: {joined(r)}")
    b.note(f"Duress PIN armed: {duress}")
    b.prompt(f"Lock the badge and enter the duress PIN {duress} on the lock screen")
    _pass_if(b, "Did the badge wipe and reboot to a clean, un-provisioned state?")
    _pass_if(b, "Was the wipe indistinguishable from a normal factory reset?")


def test_keypad(b: BadgeSerial) -> None:
    b.note("Use the on-device T9 keypad (e.g. rename or a text field).")
    b.prompt("Type a short string with umlauts using the keypad")
    _pass_if(b, "Did all characters (incl. umlauts) enter and render correctly?")


def test_plugin_ui(b: BadgeSerial) -> None:
    b.note("Maps to T-HIL07. Needs a crafted test plugin requesting a blocked pin.")
    b.prompt("Start a plugin that requests a blocked GPIO pin / undeclared capability")
    _pass_if(b, "Was the request rejected (HOST_ERR) with the badge staying responsive?")
    b.prompt("Open the plugin canvas view and press the back key (N)")
    _pass_if(b, "Did the canvas pop cleanly back without consuming all keys?")


SEMI_TESTS = [
    Test("S-FIDO2", "FIDO2 register/assert", test_fido2, category="semi",
         frs="FR-010..017", interaction="host-ctap2"),
    Test("S-OPENPGP", "OpenPGP sign/decrypt over CCID", test_openpgp, category="semi",
         frs="FR-040..043", interaction="host-ccid"),
    Test("S-BLE-XFER", "BLE vCard transfer", test_ble_transfer, category="semi",
         frs="FR-050..054", interaction="second-badge"),
    Test("S-GPG-BLE", "Cross-signature over BLE", test_gpg_ble, category="semi",
         frs="FR-044", interaction="second-badge"),
    Test("S-BLE-HID", "BLE HID keyboard", test_ble_hid, category="semi",
         frs="FR-090", interaction="host-protocol"),
    Test("S-USB-OTP", "USB keyboard / Yubico OTP", test_usb_otp, category="semi",
         frs="FR-023", interaction="host-protocol"),
    Test("S-EPAPER", "E-paper PARTIAL_LIGHT clock", test_epaper, category="semi",
         frs="FR-094", interaction="visual"),
    Test("S-PINUI", "Lock-screen lockout countdown", test_pin_ui, category="semi",
         frs="FR-002/003", interaction="button"),
    Test("S-DURESS", "Duress wipe", test_duress, category="semi",
         frs="FR-004/070", interaction="button", flags=("mutating",)),
    Test("S-KEYPAD", "Keypad / T9 input", test_keypad, category="semi",
         frs="FR-110/111", interaction="button"),
    Test("S-PLUGINUI", "Plugin sandbox / canvas", test_plugin_ui, category="semi",
         frs="FR-072/073", interaction="button"),
]
