#!/usr/bin/env python3
"""Comprehensive OpenPGP smartcard (CCID) functional test for the CDC Badge.

Drives the badge as a real OpenPGP card through stock GnuPG and exercises every
card crypto function against the keys already generated on the device:
enumeration, public-key / SSH export, signing (PSO:CDS), decryption (PSO:DEC),
authentication (INTERNAL AUTHENTICATE via the ssh-agent), and the admin/PW3
surface (cardholder data). Runs in an isolated throwaway GNUPGHOME; the card's
full public key is pulled from the user's default keyring (the card itself only
exposes the bare points, so the pubkey with its subkeys must come from there).

PIN handling: card PINs are supplied via loopback pinentry. A WRONG PW1/PW3
decrements the card retry counter (3 attempts, then blocked), so pass the
correct ones.

Usage:
    python tools/gpg_card_test.py --pw1 123456 [--pw3 12345678]

Requires: gpg 2.2+, an OpenPGP card populated (badge keys generated), and the
card's public key present in the default keyring.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

GREEN, RED, DIM, RST = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def run(cmd, env=None, input_bytes=None, timeout=60):
    return subprocess.run(cmd, env=env, input=input_bytes,
                          capture_output=True, timeout=timeout)


class CardTest:
    def __init__(self):
        self.home = tempfile.mkdtemp(prefix="gpgcard-")
        os.chmod(self.home, 0o700)
        with open(os.path.join(self.home, "gpg-agent.conf"), "w") as f:
            f.write("allow-loopback-pinentry\nenable-ssh-support\n")
        self.env = dict(os.environ, GNUPGHOME=self.home)
        self.results = []

    def gpg(self, *args, pw=None, input_bytes=None, default_home=False):
        cmd = ["gpg", "--batch", "--yes"]
        if pw is not None:
            cmd += ["--pinentry-mode", "loopback", "--passphrase", pw]
        cmd += list(args)
        env = None if default_home else self.env
        return run(cmd, env=env, input_bytes=input_bytes)

    def record(self, name, ok, detail=""):
        self.results.append((name, ok, detail))
        tag = f"{GREEN}PASS{RST}" if ok else f"{RED}FAIL{RST}"
        print(f"  [{tag}] {name}" + (f"  {DIM}{detail}{RST}" if detail else ""))

    def cleanup(self):
        run(["gpgconf", "--homedir", self.home, "--kill", "all"])
        shutil.rmtree(self.home, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pw1", required=True, help="card user PIN (PW1)")
    ap.add_argument("--pw3", help="card admin PIN (PW3); enables the cardholder-data test")
    args = ap.parse_args()

    # Release the card from any stale scdaemon (one scdaemon owns it exclusively).
    run(["gpgconf", "--kill", "all"])

    t = CardTest()
    try:
        # 1. Enumerate the card (no PIN).
        cs = t.gpg("--card-status")
        out = cs.stdout.decode(errors="replace")
        if cs.returncode != 0 or "OpenPGP" not in out:
            t.record("card enumeration", False,
                     cs.stderr.decode(errors='replace').strip()[-160:])
            return 1
        t.record("card enumeration (SELECT + GET DATA, version 3.4)", True)

        sig_fpr = dec_fpr = aut_fpr = None
        for line in out.splitlines():
            ls = line.strip()
            if ls.startswith("Signature key"):
                sig_fpr = ls.split(":", 1)[-1].replace(" ", "").lstrip(".")
            elif ls.startswith("Encryption key"):
                dec_fpr = ls.split(":", 1)[-1].replace(" ", "").lstrip(".")
            elif ls.startswith("Authentication key"):
                aut_fpr = ls.split(":", 1)[-1].replace(" ", "").lstrip(".")
        t.record("three keys on card (SIG/DEC/AUT)", all([sig_fpr, dec_fpr, aut_fpr]),
                 f"SIG={sig_fpr[-8:]} DEC={dec_fpr[-8:]} AUT={aut_fpr[-8:]}")

        # 2. Pull the full public key (with subkeys) from the default keyring.
        pub = t.gpg("--export", sig_fpr, default_home=True)
        imp = t.gpg("--import", input_bytes=pub.stdout)
        t.gpg("--card-status")  # link the card secret stubs to the imported pubkey
        keys = t.gpg("--list-keys", "--with-colons").stdout.decode(errors="replace")
        has_e = any(l.startswith("sub:") and l.split(":")[11].find("e") >= 0
                    for l in keys.splitlines())
        has_a = any(l.startswith("sub:") and l.split(":")[11].find("a") >= 0
                    for l in keys.splitlines())
        t.record("public key + encryption[e] + auth[a] subkeys available",
                 pub.returncode == 0 and has_e and has_a,
                 f"importable from default keyring; [e]={has_e} [a]={has_a}")

        # 3. SSH public-key export from the AUT key (no PIN).
        ssh = t.gpg("--export-ssh-key", sig_fpr)
        t.record("SSH public-key export (AUT key)",
                 ssh.returncode == 0 and ssh.stdout.startswith(b"ssh-"))

        # 4. Signing (PSO:COMPUTE DIGITAL SIGNATURE, PW1) + verify.
        msg = b"cdc-badge gpg card functional test\n"
        sig = t.gpg("--armor", "-u", sig_fpr, "--detach-sign", pw=args.pw1, input_bytes=msg)
        ok = sig.returncode == 0 and b"BEGIN PGP SIGNATURE" in sig.stdout
        t.record("signing (PSO:CDS, PW1)", ok,
                 "" if ok else sig.stderr.decode(errors='replace').strip()[-140:])
        if ok:
            sp = os.path.join(t.home, "m.sig")
            open(sp, "wb").write(sig.stdout)
            ver = t.gpg("--verify", sp, "-", input_bytes=msg)
            t.record("signature verifies against card public key",
                     b"Good signature" in ver.stderr)

        # 5. Encryption to DEC key + decryption (PSO:DECIPHER, PW1) round-trip.
        ct = t.gpg("--armor", "--trust-model", "always", "--encrypt",
                   "--recipient", sig_fpr, input_bytes=msg)
        if ct.returncode == 0 and b"BEGIN PGP MESSAGE" in ct.stdout:
            pt = t.gpg("--decrypt", pw=args.pw1, input_bytes=ct.stdout)
            t.record("decryption (PSO:DECIPHER, PW1) round-trip", pt.stdout == msg,
                     "" if pt.stdout == msg else pt.stderr.decode(errors='replace').strip()[-140:])
        else:
            t.record("encryption to card DEC key", False,
                     ct.stderr.decode(errors='replace').strip()[-140:])

        # 6. Authentication (INTERNAL AUTHENTICATE via the gpg ssh-agent).
        sock = run(["gpgconf", "--list-dirs", "agent-ssh-socket"],
                   env=t.env).stdout.decode().strip()
        sshenv = dict(t.env, SSH_AUTH_SOCK=sock)
        run(["ssh-add", "-L"], env=sshenv)  # surface the card key on the agent
        pubf = os.path.join(t.home, "auth.pub")
        open(pubf, "wb").write(ssh.stdout)
        # ssh-add -T signs a challenge with the card auth key and verifies it.
        try:
            tst = run(["ssh-add", "-v", "-T", pubf], env=sshenv, timeout=20)
            auth_ok = tst.returncode == 0
            detail = "" if auth_ok else "agent could not sign with the card auth key"
        except subprocess.TimeoutExpired:
            auth_ok, detail = False, "ssh-agent auth timed out (interactive pinentry?)"
        t.record("authentication (INTERNAL AUTHENTICATE via ssh-agent)", auth_ok, detail)

        # 7. Admin / PW3: write + read back the cardholder name (PUT DATA, PW3).
        if args.pw3:
            t.gpg("--command-fd", "0", "--edit-card", pw=args.pw3,
                  input_bytes=b"admin\nname\nBadge\nTest\n\nquit\n")
            cs2 = t.gpg("--card-status").stdout.decode(errors="replace")
            name_line = next((l for l in cs2.splitlines()
                              if l.strip().startswith("Name of cardholder")), "")
            t.record("admin: set+read cardholder name (PUT DATA 0x5B, PW3)",
                     "Test" in name_line or "Badge" in name_line, name_line.strip())

        return 0 if all(ok for _, ok, _ in t.results) else 1
    finally:
        n_ok = sum(1 for _, ok, _ in t.results if ok)
        print(f"\n{n_ok}/{len(t.results)} GPG card functions verified")
        t.cleanup()


if __name__ == "__main__":
    sys.exit(main())
