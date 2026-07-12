"""Host-side tests for tools/provision.py (no hardware, no real subprocesses).

Covers the acceptance criteria from the security review:
  - state machine: fresh / partially provisioned / occupied key blocks /
    wrong order / repetition / contradictory eFuse state
  - failure injection: a failing external command aborts, nothing continues
  - artifact validation: ambiguous or missing bins, oversized images
  - rotate-key: invalidate is impossible without a verified new slot and
    impossible against the active slot
  - tool preflight: esptool v4.x rejected, missing tool has a clear message

Run:  pytest tools/tests/
"""

import importlib.util
import json
import os
import subprocess
import sys
import types

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location(
    "provision", os.path.join(_HERE, "..", "provision.py"))
prov = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(prov)


# --- helpers -------------------------------------------------------------------

def make_summary(purposes=(), crypt_cnt=0, sb_en=False, harden=False,
                 mac="aa:bb:cc:dd:ee:ff"):
    """Build a minimal espefuse-summary dict for the state machine."""
    s = {"MAC": {"value": mac},
         "SPI_BOOT_CRYPT_CNT": {"value": crypt_cnt},
         "SECURE_BOOT_EN": {"value": sb_en}}
    for n in range(6):
        purpose = purposes[n] if n < len(purposes) else "USER"
        s[f"KEY_PURPOSE_{n}"] = {"value": purpose}
        s[f"BLOCK_KEY{n}"] = {"value": "00" * 32 if purpose == "USER"
                              else "ab" * 32}
    for name in prov.HARDEN_EFUSES:
        s[name] = {"value": harden}
    return s


class FakeCtx:
    """Just enough of prov.Ctx for the LOCK_STEPS done()-lambdas."""

    def __init__(self, tmp_path, summary, manifest=None):
        self.dir = str(tmp_path)
        self.summary = summary
        self.manifest = manifest or {"keys": {}, "stages": {}}

    def path(self, name):
        return os.path.join(self.dir, name)


def status(ctx):
    return prov._lock_status(ctx)


class FakeBadge:
    """Scripted serial badge: maps command prefixes to response lines."""

    def __init__(self, responses):
        self.responses = responses
        self.sent = []

    def connect(self):
        return self

    def close(self):
        pass

    def authenticate(self):
        pass

    def command(self, cmd, timeout=5):
        self.sent.append(cmd)
        for prefix, resp in self.responses.items():
            if cmd.startswith(prefix):
                return list(resp)
        return []


def ns(**kw):
    base = dict(dry_run=False, port="/dev/MOCK", pin=None,
                i_understand_this_is_irreversible=True,
                new_slot=1, verify=False, invalidate_old_slot=None)
    base.update(kw)
    return types.SimpleNamespace(**base)


@pytest.fixture()
def sandbox(tmp_path, monkeypatch):
    """Redirect all filesystem side effects into tmp_path."""
    monkeypatch.setattr(prov, "SECRETS_DIR", str(tmp_path / "secrets"))
    monkeypatch.setattr(prov, "PAIRING_HEADER",
                        str(tmp_path / "pairing_key_custom.h"))
    monkeypatch.setattr(prov, "_ensure_gitignore_secrets", lambda: None)
    return tmp_path


# --- tool preflight --------------------------------------------------------------

def _fake_version_run(stdout, returncode=0):
    def fake(cmd, capture_output=True, text=True, timeout=None):
        return types.SimpleNamespace(returncode=returncode,
                                     stdout=stdout, stderr="")
    return fake


def test_preflight_rejects_v4(monkeypatch):
    prov._TOOL_CACHE.clear()
    monkeypatch.setattr(prov.subprocess, "run",
                        _fake_version_run("esptool.py v4.9.0"))
    with pytest.raises(SystemExit) as e:
        prov._resolve_tool("esptool")
    assert "esptool>=5" in str(e.value)


def test_preflight_accepts_v5(monkeypatch):
    prov._TOOL_CACHE.clear()
    monkeypatch.setattr(prov.subprocess, "run",
                        _fake_version_run("esptool v5.2.0"))
    assert prov._resolve_tool("esptool")


def test_preflight_missing_tool(monkeypatch):
    prov._TOOL_CACHE.clear()

    def boom(cmd, **kw):
        raise OSError("no such file")

    monkeypatch.setattr(prov.subprocess, "run", boom)
    with pytest.raises(SystemExit) as e:
        prov._resolve_tool("espefuse")
    assert "pip install" in str(e.value)


# --- run_cmd failure injection ----------------------------------------------------

def test_run_cmd_dies_on_failure(monkeypatch):
    monkeypatch.setattr(prov.subprocess, "run", _fake_version_run(
        "boom", returncode=2))
    with pytest.raises(SystemExit) as e:
        prov.run_cmd(["some", "cmd"], dry_run=False)
    assert "exit 2" in str(e.value)


def test_failed_keygen_records_nothing(sandbox, monkeypatch):
    ctx = FakeCtx(sandbox, make_summary())
    ctx.dry = False
    ctx.espsecure = ["espsecure"]
    monkeypatch.setattr(prov.subprocess, "run",
                        _fake_version_run("fail", returncode=1))
    with pytest.raises(SystemExit):
        prov._step_gen_fe(ctx)
    assert ctx.manifest["keys"] == {}
    assert ctx.manifest["stages"] == {}


# --- state machine ------------------------------------------------------------------

def test_fresh_chip_starts_at_step_zero(sandbox):
    rows, first_open = status(FakeCtx(sandbox, make_summary()))
    assert first_open == 0
    assert not any(done for _k, _d, done in rows)


def test_partial_state_resumes_correctly(sandbox):
    ctx = FakeCtx(sandbox, make_summary(purposes=["XTS_AES_128_KEY"]))
    open(ctx.path("fe_key.bin"), "wb").close()
    open(ctx.path("sb_key.pem"), "wb").close()
    open(ctx.path("sb_digest.bin"), "wb").close()
    rows, first_open = status(ctx)
    assert [d for _k, _de, d in rows[:3]] == [True, True, True]
    assert first_open == 3  # burn_sb_digest is next
    prov._check_consistency(rows, first_open)  # consistent -> no exit


def test_done_steps_are_never_repeated(sandbox):
    ctx = FakeCtx(sandbox, make_summary())
    open(ctx.path("fe_key.bin"), "wb").close()
    rows, first_open = status(ctx)
    assert rows[0][2] is True
    assert first_open == 1  # step 0 done, never selected again


def test_contradictory_state_refuses(sandbox):
    # SECURE_BOOT_EN already burned but no keys generated -> refuse.
    ctx = FakeCtx(sandbox, make_summary(crypt_cnt=7, sb_en=True))
    ctx.manifest["stages"]["flash_release"] = {"done": True}
    rows, first_open = status(ctx)
    assert first_open == 0
    with pytest.raises(SystemExit) as e:
        prov._check_consistency(rows, first_open)
    assert "inconsistent" in str(e.value)


def test_occupied_key_blocks_refuse(sandbox):
    taken = ["XTS_AES_256_KEY_1"] * 6
    with pytest.raises(SystemExit) as e:
        prov._free_key_block(make_summary(purposes=taken), "XTS_AES_128_KEY")
    assert "no free eFuse key block" in str(e.value)


def test_free_key_block_skips_occupied(sandbox):
    summary = make_summary(purposes=["SECURE_BOOT_DIGEST0"])
    assert prov._free_key_block(summary, "XTS_AES_128_KEY") == 1


# --- artifact validation --------------------------------------------------------------

def test_missing_bin_dies(tmp_path):
    with pytest.raises(SystemExit) as e:
        prov._find_bin(str(tmp_path), "bootloader")
    assert "no *bootloader*" in str(e.value)


def test_ambiguous_bins_die(tmp_path):
    (tmp_path / "firmware-a.bin").write_bytes(b"x")
    (tmp_path / "firmware-b.bin").write_bytes(b"x")
    with pytest.raises(SystemExit) as e:
        prov._find_bin(str(tmp_path), "firmware")
    assert "ambiguous" in str(e.value)


def test_derived_artifacts_are_ignored(tmp_path):
    (tmp_path / "firmware.bin").write_bytes(b"x")
    (tmp_path / "firmware.bin.signed").write_bytes(b"x")
    (tmp_path / "firmware.bin.enc").write_bytes(b"x")
    assert prov._find_bin(str(tmp_path), "firmware").endswith("firmware.bin")


def test_oversized_partition_table_dies(tmp_path):
    big = tmp_path / "partitions.bin"
    big.write_bytes(b"\0" * 0x2000)  # > 0x1000 region at 0x8000
    with pytest.raises(SystemExit) as e:
        prov._check_region(str(big), 0x8000, "partition table")
    assert "overwrite the next partition" in str(e.value)


def test_oversized_bootloader_dies(tmp_path):
    big = tmp_path / "bootloader.bin.signed"
    big.write_bytes(b"\0" * 0x9000)  # > 0x8000 region at 0x0
    with pytest.raises(SystemExit):
        prov._check_region(str(big), 0x0, "bootloader")


def test_fitting_image_passes(tmp_path):
    ok = tmp_path / "partitions.bin"
    ok.write_bytes(b"\0" * 0xC00)
    prov._check_region(str(ok), 0x8000, "partition table")  # no exit


# --- rotate-key gating -----------------------------------------------------------------

CHIP_ID = "01234567ABCDEF99"


def _badge(profile_slot, chip_id=CHIP_ID, session_ok=True):
    return FakeBadge({
        "VERSION": [f"Profile: 0x02 debug=0 secure_serial=1 provisioning=1 "
                    f"pairing_slot={profile_slot} flash_enc=0 secure_boot=0"],
        "TR01 INFO": [f"Chip ID: {chip_id}"],
        "TR01 SESSION": ["OK: Session started" if session_ok
                         else "ERROR: Session start failed"],
        "TR01 PAIR_INVALIDATE": ["OK: pairing slot permanently invalidated"],
        "TR01 PAIR_WRITE": ["OK: pairing key written to slot 1"],
    })


def _with_badge(monkeypatch, badge):
    monkeypatch.setattr(prov, "BadgeSerial", lambda port, pin: badge)


def _manifest_dir(chip_id=CHIP_ID):
    d = prov._tr01_dir(chip_id)
    return d


def test_invalidate_without_verify_refused(sandbox, monkeypatch):
    _with_badge(monkeypatch, _badge(profile_slot=1))
    d = _manifest_dir()
    prov.save_manifest(d, {"keys": {}, "stages": {},
                           "pairing": {"slot": 1, "written": True,
                                       "verified": False}})
    with pytest.raises(SystemExit) as e:
        prov.cmd_rotate_key(ns(invalidate_old_slot=0))
    assert "never verified" in str(e.value)


def test_invalidate_active_slot_refused(sandbox, monkeypatch):
    _with_badge(monkeypatch, _badge(profile_slot=1))
    d = _manifest_dir()
    prov.save_manifest(d, {"keys": {}, "stages": {},
                           "pairing": {"slot": 1, "written": True,
                                       "verified": True}})
    with pytest.raises(SystemExit) as e:
        prov.cmd_rotate_key(ns(invalidate_old_slot=1))
    assert "currently authenticating" in str(e.value)


def test_verify_records_success(sandbox, monkeypatch):
    _with_badge(monkeypatch, _badge(profile_slot=1))
    d = _manifest_dir()
    prov.save_manifest(d, {"keys": {}, "stages": {},
                           "pairing": {"slot": 1, "written": True,
                                       "verified": False}})
    prov.cmd_rotate_key(ns(verify=True))
    m = prov.load_manifest(d)
    assert m["pairing"]["verified"] is True


def test_verify_wrong_slot_refused(sandbox, monkeypatch):
    # Badge still authenticates via slot 0 -> verify must fail.
    _with_badge(monkeypatch, _badge(profile_slot=0))
    d = _manifest_dir()
    prov.save_manifest(d, {"keys": {}, "stages": {},
                           "pairing": {"slot": 1, "written": True,
                                       "verified": False}})
    with pytest.raises(SystemExit) as e:
        prov.cmd_rotate_key(ns(verify=True))
    assert "expected new slot" in str(e.value)


def test_verify_chip_mismatch_refused(sandbox, monkeypatch):
    badge = _badge(profile_slot=1)
    _with_badge(monkeypatch, badge)
    # Manifest belongs to ANOTHER chip: chip dir resolves from the live badge,
    # so plant a manifest with a mismatching recorded id inside that dir.
    d = _manifest_dir(CHIP_ID)
    prov.save_manifest(d, {"keys": {}, "stages": {},
                           "pairing": {"slot": 1, "written": True,
                                       "verified": False}})
    # First INFO call resolves the dir; later INFO call must mismatch.
    calls = {"n": 0}
    orig = badge.command

    def flaky(cmd, timeout=5):
        if cmd.startswith("TR01 INFO"):
            calls["n"] += 1
            if calls["n"] > 1:
                return ["Chip ID: FFFFFFFFFFFFFFFF"]
        return orig(cmd, timeout=timeout)

    badge.command = flaky
    with pytest.raises(SystemExit) as e:
        prov.cmd_rotate_key(ns(verify=True))
    assert "chip id mismatch" in str(e.value)


def test_soft_phase_refuses_existing_key(sandbox, monkeypatch):
    _with_badge(monkeypatch, _badge(profile_slot=0))
    monkeypatch.setattr("builtins.input", lambda *_: "WRITE PAIRING SLOT 1")
    d = _manifest_dir()
    with open(os.path.join(d, "sync_key.json"), "w") as f:
        f.write("{}")
    with pytest.raises(SystemExit) as e:
        prov.cmd_rotate_key(ns())
    assert "Refusing to overwrite" in str(e.value)


# --- misc guards -----------------------------------------------------------------------

def test_detect_port_multiple_dies(monkeypatch):
    monkeypatch.setattr(prov.glob, "glob",
                        lambda pat: ["/dev/ttyACM0", "/dev/ttyACM1"]
                        if "ttyACM" in pat else [])
    with pytest.raises(SystemExit) as e:
        prov.detect_port()
    assert "use --port" in str(e.value)


def test_manifest_key_hash_mismatch_dies(sandbox):
    d = prov._make_secrets_dir("esp-test")
    key = os.path.join(d, "fe_key.bin")
    with open(key, "wb") as f:
        f.write(b"AAAA")
    manifest = {"keys": {}, "stages": {}}
    prov._record_key(manifest, "fe_key", key)
    with open(key, "wb") as f:
        f.write(b"BBBB")  # tampered/replaced
    with pytest.raises(SystemExit) as e:
        prov._check_key(manifest, "fe_key", key)
    assert "does NOT match" in str(e.value)


def test_expect_ok_dies_on_error(capsys):
    with pytest.raises(SystemExit):
        prov._expect_ok(["ERROR: pairing write failed"], "PAIR_WRITE")


def test_parse_profile_extracts_fields():
    p = prov._parse_profile(
        ["Firmware: 0.8.1",
         "Profile: 0x02 debug=0 secure_serial=1 provisioning=0 "
         "pairing_slot=1 flash_enc=1 secure_boot=1"])
    assert p["flash_enc"] == "1" and p["pairing_slot"] == "1"
