# Quickstart & Validation: `cdc-badge-development`

Runnable validation scenarios proving the feature works end to end. Each maps to a Success Criterion (SC) in `spec.md`. Detailed shapes live in `contracts/` and `data-model.md`; this guide is the run/validate checklist, not implementation code.

> **Output target reminder**: everything below happens in the **new** `cdc-badge-development` repo, not in `cdc-badge-os`. The new repo is created first (see QS0).

## Prerequisites

- A code editor (VS Code), git, and Python — `EASY.md` shows how to install these per OS (brew / apt / winget). The bootstrap then runs as `python scripts/setup.py` on every OS — no PowerShell.
- Network access for the first provisioning.
- A physical CDC Badge over USB — required only for QS2/QS3 (flash/run/debug), not for build-only QS1.
- Docker — only for the container alternative (QS8) and CI (QS7).

## QS0 — Repository exists (maintainer, one-time)

1. Local repo at `~/GIT/cdc-badge-development` initialised; two submodules under `vendor/`.
2. GitHub remote `krim404/cdc-badge-development` and GitLab remote `library/cdc-badge-development` created, each with a description/about set.

**Expected**: `git -C ~/GIT/cdc-badge-development submodule status` lists both submodules at pinned commits; both remotes reachable.

## QS1 — Trivial onboarding to a green build (SC-001, SC-003)

1. Open the repo in VS Code → accept recommended extensions.
2. Run the README's single first action (the `Setup` task / `scripts/setup.*`).
3. Run `badge build hello_world` (or the `Build plugin` task) against an example.

**Expected**: setup completes with no manual per-dependency steps; an example compiles to `target/wasm32-unknown-unknown/release/<id>.wasm` (+ `wasm-opt`); first build reached in ≤3 actions.

## QS2 — Build, flash, run, read logs on the badge (SC-002, SC-010)

1. `badge new my_first` → a buildable plugin in `plugins/my_first/`.
2. With the badge connected: `badge flash my_first --start --monitor` (or the `Flash plugin` task).

**Expected**: the plugin builds, uploads (badge `OK`), starts on the device, and its log lines stream live. No manual chunking/CRC/device-path steps.

## QS3 — AI agent drives flash + serial directly (SC-004, SC-010)

In each of Claude Code, Codex, and opencode, with the repo open:

1. Ask the agent to scaffold a plugin → it invokes the `cdc-badge-plugin-dev` skill and uses `badge new`/correct layout.
2. Ask it to flash and run → it calls `badge flash … --start` directly on the host.
3. Ask it to debug a deliberately broken plugin → it reads `badge monitor` output and explains the rejection (capability/manifest/host-API/size/auth) using the skill's pitfalls.

**Expected**: each agent reaches the skill and gives correct, real commands (no invented ones); the agent flashes and reads logs with no manual human steps.

## QS4 — Spec-driven development active on first open (SC-005)

In each agent: list workflow commands and start a spec.

**Expected**: `specify/plan/tasks/implement`-style commands are present and usable with no `specify init`; the committed `.specify/` templates + constitution apply automatically.

## QS5 — Knowledge is current and updatable (SC-006, SC-008)

1. From `knowledge/index.md`, follow cross-links to the host-API reference, SDK/manifest docs, and a firmware spec under `vendor/`.
2. Maintainer: `git submodule update --remote vendor/cdc-badge-plugins && git commit`.

**Expected**: all cross-links resolve within the repo; the update advances the vendored knowledge with no hand-copying and no course-material rewrite.

## QS6 — Webflasher fallback without host USB (SC-009)

From a host/container without direct USB access, open the WebSerial webflasher (`vendor/cdc-badge-plugins/webflasher` or the upstream-hosted page) and install the built plugin.

**Expected**: upload succeeds via the browser with no USB passthrough.

## QS7 — Container image publishes via both pipelines (SC-011)

With credentials configured: push to the GitHub default branch (Docker Hub) and the GitLab default branch (registry.krim.dev).

**Expected**: each pipeline builds the `Dockerfile` and pushes `cdc-badge-development:latest`; without credentials, each no-ops and local use is unaffected.

## QS8 — Container alternative builds (SC-007 adjacent)

"Reopen in Container" → run `badge build hello_world` inside the container.

**Expected**: the same example builds; uploading then uses the QS6 webflasher fallback.

## QS9 — Cross-platform verification CI (SC-013)

Push any change; the `ci.yml` matrix runs on `windows-latest`, `macos-latest`, `ubuntu-latest`.

**Expected**: each OS provisions the toolchain (bootstrap) and builds the example plugin green — the maintainer's automated stand-in for manually testing Windows.

## Classical-only sanity (SC-007)

Ignore all AI/SDD features; run QS1 + QS2 only.

**Expected**: build/flash/debug loop works with no agent involvement.
