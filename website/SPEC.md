# Documentation Website — Specification (SDD)

This is the **authoritative specification** for the CDC Badge OS documentation website. It is written
*before* the content and is the contract every page is verified against. It defines the page tree, the
content rules, the source-of-truth hierarchy, the verification protocol, and the acceptance criteria.

> Status: DRAFT pending sign-off. Nothing in the page tree is published content; entries are *targets*
> with their primary source files, to be verified during authoring.

---

## 1. Purpose & Scope

Replace the outdated, loose `docs/*.md` files and the bare Doxygen page with a generated, host-agnostic,
visually polished documentation site covering:

- the **real feature set** of the badge (what it does),
- **how to use** each feature,
- the **security measures and background processes** (on-chip key generation, attestation key, PIN/lockout
  semantics, duress/self-destruct, secure element),
- a **freshly written** developer and protocol documentation set.

Out of scope: source-code-level commentary (that is Doxygen's job, linked under `/api/`).

---

## 2. Non-negotiable Content Rules

1. **Truth only.** A statement may be published only if it is verifiable from an authoritative source
   (see §3). No guessing, no inference presented as fact, nothing false.
2. **Evidence required.** Every factual claim must trace to a `file:line` captured at authoring time and
   recorded in the page's Evidence Ledger (§5). Unbacked text is not published.
3. **Gaps are asked or dropped.** Anything not verifiable is a `GAP`: ask the user, omit it, or rewrite to
   the verified subset. A `GAP` is never published as fact (§5).
4. **No GUI drawings.** No screen mockups, no ASCII art of the display, no pixel layouts. On-device
   navigation is described only as text breadcrumbs, e.g. *Tools → Bluetooth → Pair device*.
5. **Honest status.** Features still untested-on-hardware (the BLE serial console and the GPG
   cross-sign send path) are labelled as such and never presented as finished. BLE vCard, BLE HID and
   the cdc_msg framework were hardware-verified 2026-06-18.
6. **No version bumps.** Do not change any firmware/HOST_API/schema version while writing docs.
7. **English.** All site content is English (project convention). German UI labels may be quoted as data.
8. **Capacities from source.** Counts/limits (credential/account/entry capacities, slot ranges) are taken
   directly from `main/tropic_slot_map.h` and module headers, never from prose summaries.

---

## 3. Source-of-Truth Hierarchy (descending authority)

1. **Firmware source + i18n tables** — authoritative.
   - i18n: `components/cdc_ui/src/I18n.cpp` (`kCoreStrings[]`), `assets/i18n/lang_de.json`, per-module
     English tables.
   - Maps/defaults: `main/tropic_slot_map.h`, `main/module_defaults.h`, `main/CMakeLists.txt` (MODULES).
   - Module headers/sources under `components/`.
   - Plugin host API: `components/plugin_manager/include/plugin_manager/host_api.h` (canonical).
2. **Build/CI/config** — `platformio.ini`, `.github/`, `.forgejo/`, `.gitlab-ci.yml`, `Doxyfile`.
3. **Existing `docs/*.md` and `skill/*.md`** — HINTS ONLY, treated as **stale**. Never copied without
   re-verification against (1).

---

## 4. Verified module set (authoritative, `main/CMakeLists.txt` + `module_defaults.h`)

Default-enabled: `mod_2fa`, `mod_fido2`, `mod_password`, `mod_gpg`, `mod_sao`, `mod_vcard`,
`mod_ble_serial`, `mod_nvsedit`, `mod_blehid`, `mod_vfat`.
Default-disabled (manual enable): `mod_usbhid`, `mod_otphid`.

---

## 5. Per-page Verification Protocol

Each content page `X` carries a sibling, non-published **Evidence Ledger** `X.evidence.md` with one row per
factual claim:

```
| Claim (verbatim or paraphrased) | Source file:line | Tag |
```

- `Tag = VERIFIED` → has a concrete `file:line` that supports the claim. Publishable.
- `Tag = GAP` → could not be verified. Resolve by: (a) ask user, (b) omit, (c) rewrite to verified subset.
  A `GAP` row must be resolved before the page ships; it is never published as fact.

**Authoring pipeline per page (automatable):**
1. Read the page's primary source files (and follow references).
2. Draft prose; for each claim add an Evidence Ledger row with `file:line`.
3. **Adversarial verification pass** (independent second reader): re-check every row against the code; flag
   anything unsupported or overstated. Crypto/security claims (algorithms, key lengths, KDF iterations,
   guarantees) get the strictest scrutiny — confirm exact values at the code or drop the number.
4. Resolve all `GAP`s (ask/omit/rewrite). Only then mark the page done.

---

## 6. Site Technical Conventions

- **Generator:** Astro Starlight `0.40.0` (Astro `^6.4.5`); re-confirm latest from npm at build time and pin.
- **Project location:** `website/` in this repo. Build: `npm ci && npm run build` → static `website/dist/`.
- **Portable base path:** `astro.config.mjs` reads `SITE_BASE` (default `/cdc-badge-os/`) and `SITE_URL`
  from env so the same source deploys to GitHub/Codeberg/GitLab project Pages (subpath) or a custom domain
  (`/`). All internal links use Starlight's base-aware routing.
- **Published topology (Layout A, recommended — confirm before CI step):**
  - `/` → Starlight docs (front door)
  - `/flasher/` → existing web flasher + `firmware.bin` + `manifest*.json`
  - `/api/` → Doxygen code reference
  Fallback Layout B (if external links to the root flasher must be preserved): flasher stays at `/`,
  docs at `/docs/`, Doxygen at `/api/`.
- **Theming ("must look good"):** custom logo (light/dark), badge-matched color palette via `customCss`,
  `template: splash` landing with hero + CTAs, Fontsource font, `editLink`, repo/social links.
- **Doxygen:** built by existing CI from `Doxyfile`; Tier-3 pages deep-link concrete classes/files under
  `/api/` (e.g. `IBluetoothController`, `ISecureElement`, `host_api.h`).

---

## 7. Information Architecture — Three Tiers

Top-level navigation = three tiers. Tables list each page's slug, working title, primary source files to
verify against, and status (`VERIFY` = exists, re-verify; `NEW` = no current doc; `WIP` = feature marked
work-in-progress).

### Tier 1 — Getting Started (Einsteiger)

| Slug | Title | Primary sources | Status |
|------|-------|-----------------|--------|
| `index` | What is the CDC Badge? (splash) | `README.md`, `cdc_hal/hw_config.h` | VERIFY |
| `start/overview` | Hardware & capabilities overview | `README.md`, `main/tropic_slot_map.h`, `cdc_hal/` | VERIFY |
| `start/first-flash` | Flashing the firmware | web-flasher, `.gitlab-ci.yml`/`deploy-pages.yml`, `tools/flash_firmware.py`, badge_serial_bootloader | VERIFY |
| `start/first-boot` | First boot, PIN setup, unlocking | `cdc_os_ui/src/AppUi.cpp`, `cdc_views/PinEntryView.h` | VERIFY |
| `start/keypad-navigation` | Keypad & input methods | `cdc_hal/IKeypad.h`, `cdc_views/{T9InputView,SliderView,DateInputView,TimeInputView,ListView}.h` | VERIFY |
| `guide/lock-and-pin` | Lock screen, PIN, change PIN | `AppUi.cpp`, `cdc_core` PinManager, `PinEntryView.h` | VERIFY |
| `guide/fido2-webauthn` | Passkeys / WebAuthn / U2F | `components/mod_fido2/**` | VERIFY |
| `guide/two-factor` | 2FA: TOTP / HOTP / challenge-response | `components/mod_2fa/**` | VERIFY |
| `guide/password-vault` | Password vault | `components/mod_password/**` | VERIFY |
| `guide/gpg-ssh` | GPG / OpenPGP card & SSH | `components/mod_gpg/**` | VERIFY |
| `guide/vcard` | vCard exchange | `components/mod_vcard/**`, `components/cdc_msg/**` | VERIFIED |
| `guide/bluetooth` | Bluetooth: pairing & bonds | `cdc_hal/IBluetoothController.h`, `cdc_os_ui/BluetoothMenuUi.cpp` | VERIFY |
| `guide/wifi-time` | WiFi & time sync | `cdc_os_ui/WifiMenuUi.cpp`, i18n `core.wifi_*`/`core.ntp_*` | VERIFY |
| `guide/auto-type` | Auto-type via HID (BLE/USB) | `mod_blehid`, `mod_usbhid` | VERIFIED |
| `guide/backup-restore` | Encrypted backup & restore | `cdc_os_ui` BackupManager, Expert menu | VERIFY |
| `guide/settings` | Settings | `AppUi.cpp` settings menu, i18n `core.*` | VERIFY |
| `guide/power-sleep` | Power, battery & sleep | `cdc_os_ui/SleepManager.cpp`, `AppUi.cpp` status icons | VERIFY |
| `security/overview` | How your secrets are protected | `README.md`, `docs/SECURITY.md` (re-verify) | VERIFY |
| `security/secure-element-keys` | Secure element & automatic key generation | `main/tropic_slot_map.h`, `cdc_hal/ISecureElement.h`, `mod_fido2`/`mod_gpg` keygen | VERIFY |
| `security/attestation` | FIDO2 attestation key / AAGUID | `mod_fido2/src/ctap2.cpp` | VERIFY |
| `security/pin-lockout` | PIN & lockout semantics | PinManager, `mod_gpg` PW1/PW3, `README.md` | VERIFY |
| `security/duress` | Duress PIN / self-destruct | `AppUi.cpp`/Expert menu self-destruct path | VERIFY |
| `security/caveats` | Beta status, data-loss, WIP, debug mode | `README.md`, `CLAUDE.md`, `feature_flags.h` | VERIFY |

### Tier 2 — Intermediate (Power-User)

| Slug | Title | Primary sources | Status |
|------|-------|-----------------|--------|
| `power/index` | Power-user overview | — | NEW |
| `power/plugins-overview` | Plugins (WASM/WAMR): what & sandbox | `components/plugin_manager/**`, `components/wamr_runtime/` | VERIFY |
| `power/plugins-install` | Installing plugins | web installer, `tools/upload.py`, `PLUGIN` serial cmds | VERIFY |
| `power/plugins-manage` | Managing: start/stop, background, autoload | `plugin_manager` lifecycle, PluginListView | VERIFY |
| `power/plugins-capabilities` | What plugins can do (capabilities) | `host_api.h`, `CapabilityChecker.cpp` | VERIFY |
| `power/serial-console` | Serial command interface | `components/serial_cmd/**`, per-module command tables | VERIFY |
| `power/expert-menu` | Expert menu | `cdc_os_ui/ExpertMenuUi.cpp` | VERIFY |
| `power/languages` | Languages & adding a language | `cdc_ui/I18n.cpp`, `assets/i18n/`, CLAUDE.md i18n rules | VERIFY |
| `power/storage-tools` | vFAT explorer & NVS editor | `mod_vfat`, `mod_nvsedit` | VERIFY |
| `power/companion-tools` | Companion tools | `tools/*.py` | VERIFY |

### Tier 3 — Pro / Developer

| Slug | Title | Primary sources | Status |
|------|-------|-----------------|--------|
| `dev/index` | Developer overview & repo topology | `CLAUDE.md`, `README.md` | VERIFY |
| `dev/architecture` | Architecture & components | `components/cdc_core/**`, `cdc_hal/**`, `cdc_ui/**` | VERIFY |
| `dev/build-system` | Build, flash, partitions, memory | `platformio.ini`, partition tables, `CLAUDE.md` | VERIFY |
| `dev/module-development` | Developing a module | `cdc_core/IModule.h`, `ModuleRegistry`, MODULES list | VERIFY (renew `MODULE_DEVELOPMENT.md`) |
| `dev/ui-framework` | UI framework & display pipeline | `cdc_ui/**`, `cdc_views/**`, render printText/drawText | VERIFY (renew `UI_SYSTEM_ANALYSIS.md`/`UI_FLOWS.md`) |
| `dev/plugin-sdk` | Plugin SDK (Rust) & manifest | `host_api.h`, `PluginManifest.*`, sibling SDK repo | VERIFY (renew `PLUGIN_DEVELOPMENT.md`) |
| `dev/host-api` | Host API contract & families | `host_api.h`, `WamrImports.cpp`, `host_api_*.cpp` | VERIFY |
| `dev/secure-element` | TROPIC01 slot map & ISecureElement | `main/tropic_slot_map.h`, `cdc_hal/ISecureElement.h` | VERIFY |
| `dev/proto/message-transfer` | cdc_msg message-transfer protocol | `components/cdc_msg/**` | NEW (replaces `ble_vcard_protocol.md`) |
| `dev/proto/vcard` | vCard over cdc_msg | `mod_vcard`, `cdc_msg` | NEW |
| `dev/proto/gpg-cross-signing` | GPG cross-signing protocol | `mod_gpg` xsig sources | VERIFY (renew `CROSS_SIGNING.md`) |
| `dev/proto/openpgp-ccid` | OpenPGP smartcard / CCID | `components/openpgp/**`, `mod_gpg` | VERIFY (renew `GPG.md`) |
| `dev/proto/serial-commands` | Serial command reference | `serial_cmd`, module command tables | VERIFY (renew `SERIAL_COMMANDS.md`) |
| `dev/proto/backup-format` | Backup container format | BackupManager source | VERIFY |
| `dev/proto/fido2-ctap` | FIDO2 / CTAP specifics & AAGUID | `mod_fido2/src/ctap2.cpp` | NEW |
| `dev/proto/otp-hid-cr` | Yubico OTP HID challenge-response | `mod_otphid/**` | NEW |
| `dev/api-reference` | Code reference (Doxygen) | `Doxyfile`, `/api/` | VERIFY |

---

## 8. Known Gaps / Open Questions (ask, don't guess)

- Backup crypto exact parameters (KDF iteration count, AES variant/mode): verify at source or omit the number.
- FIDO2: attestation self-signed vs CA-backed; LargeBlob / BioEnrollment / AuthnConfig — implemented or stub?
- On-hardware maturity of the BLE serial console (vCard, BLE HID and cdc_msg verified 2026-06-18).
- Password masking behaviour in edit view; precise T9 commit-timeout behaviour.
- Which modules besides vCard actually use `cdc_msg`; whether plugins can use it.
- Layout A vs B decision (flasher URL relocation) before touching CI.

---

## 9. Acceptance Criteria (Definition of Done)

- `npm run build` produces a static site cleanly, both with `SITE_BASE=/cdc-badge-os/` (subpath) and `/`.
- Three-tier navigation complete; splash landing explains "what is this" and links the web flasher + repos.
- Every published factual claim has a `VERIFIED` Evidence Ledger row; adversarial pass leaves no open
  unverified claim; WIP features clearly labelled; no GUI drawings anywhere.
- Doxygen `/api/` reachable in the assembled build and deep-linked from Tier-3 pages.
- All three CI pipelines extended consistently (verified locally by reproducing the assemble step).
- Visual sign-off by the user.

---

## 10. Execution Phases

See `/Users/krim/.claude/plans/ich-habe-als-kritikpunkt-eventual-allen.md`. Order: Spec (this file) →
scaffold → theming/landing → content (Tier 1→2→3, per-page pipeline) → Doxygen integration → CI extension
→ replace old docs (approval-gated).
