# Phase 1 Data Model: `cdc-badge-development`

This repository has no runtime database. The "entities" are the configuration and asset artifacts the repo is composed of, their fields, relationships, and validation rules (derived from the spec's Key Entities and Functional Requirements).

## 1. Onboarding Environment

The reproducible definition of the toolchain and editor setup.

| Field | Description | Validation |
|-------|-------------|------------|
| `scripts/setup.py` (+ optional thin `scripts/setup.sh` wrapper for Unix) | Host-native bootstrap (primary), cross-platform Python; **no authored PowerShell** | One non-interactive command (`python scripts/setup.py`) on every OS; idempotent; installs rustup+target, pinned `wasm-opt`, venv+`pyserial`, inits submodules; exits non-zero on failure with a clear message |
| `.devcontainer/devcontainer.json` | Container alternative | Pulls the published image; `postCreateCommand` runs submodule init; no host-USB assumption |
| `Dockerfile` | Dev image definition | Builds Rust+`wasm32`+`wasm-opt`+python venv; pinned versions; used by both CI pipelines |
| `.vscode/extensions.json` | Recommended extensions | rust-analyzer, even-better-toml, a serial monitor; offered on open |
| `.vscode/tasks.json` | Editor tasks | Setup / Build / Flash / Monitor; each maps to a `badge` CLI subcommand |
| pinned versions | Toolchain pins | Rust from `vendor/cdc-badge-plugins/rust-toolchain.toml`; `wasm-opt` Binaryen pin; `pyserial>=3.5` |

**Relationships**: provisions → the Rust/WASM/Python toolchain consumed by *Automated Flash & Serial Tooling* and *Plugin Workspace Project*. The container alternative builds the same image the *CI Pipelines* publish.

**Isolation rule** (FR-005): host-native deps live in a project venv + per-user rustup; the host system is not polluted.

## 2. Vendored Knowledge Sources

| Field | Description | Validation |
|-------|-------------|------------|
| `vendor/cdc-badge-os` | Firmware submodule | Pinned commit; provides canonical `host_api.h`, `tools/upload.py`, `website/.../docs/dev/*`, `specs/` |
| `vendor/cdc-badge-plugins` | Plugin SDK submodule | Pinned commit; provides SDK crate, examples, `plugin_template_rust`, `docs/manifest_schema.md`, `webflasher/` |
| `.gitmodules` | Submodule declaration | Both entries present; auto-init by bootstrap & devcontainer postCreate |
| update action | `git submodule update --remote vendor/<name>` | Single documented command; advances pin; never edits vendored files |

**Relationships**: read-only source for *Knowledge Hub* cross-links and *Assistant Skill* facts. `host_api.h` is consumed, never forked (Constitution V, FR-027).

## 3. Plugin Workspace Project (student artifact)

A copy-and-rename starting point under `plugins/`.

| Field | Description | Validation |
|-------|-------------|------------|
| `Cargo.toml` | Crate manifest | `crate-type = ["cdylib"]`; depends on the SDK crate; release profile `opt-level="z"`, `lto`, `panic="abort"`, `strip` |
| `src/lib.rs` | Source skeleton | `#![no_std]`; `plugin_main!()`; lifecycle exports (`plugin_init/deinit/on_enter/on_exit`) build unmodified |
| `meta.json` | Plugin manifest | See *Plugin Manifest* below |
| `<id>.lang.json` | Optional translations | Same schema as upstream; optional |

**State transitions** (the loop the student/agent drives): `created → built (.wasm) → uploaded → started → running → (stopped|deleted)`.

## 4. Plugin Manifest (`meta.json`)

Schema owned upstream (`vendor/cdc-badge-plugins/docs/manifest_schema.md`); the repo references it, does not redefine it. Required fields enforced by the starter and by `badge` before upload:

| Field | Rule |
|-------|------|
| `id` | `[a-z][a-z0-9_]{1,31}`; filename stem |
| `version` | SemVer `MAJOR.MINOR.PATCH` |
| `host_api_level_min` | `MAJOR.MINOR`; must be ≤ the firmware's level or the badge rejects load |
| `linear_memory_kb` | 16–1024 |
| `capabilities` | Gates (wifi/ble/http/…); named resources (`rmem`/`ecc`/`gpio_pins`/…); GPIO must avoid the hard-block list |
| `i18n` | English fallbacks under `i18n.meta`/`i18n.strings` |

## 5. Automated Flash & Serial Tooling (`tools/badge.py`)

| Field | Description | Validation |
|-------|-------------|------------|
| subcommands | `new`, `build`, `test`, `flash`, `monitor`, `list`, `start`, `stop`, `delete` | Non-interactive, scriptable, agent-invokable (FR-035) |
| `build` | cargo + `wasm-opt` | Emits `<id>.wasm`; reports artifact path |
| `test` | `cargo test -p <name>` (host target) | The TDD loop for plugin logic (FR-041) |
| `flash` | build-if-needed + `upload.py` | Single command; `--start`, `--pin`, `--monitor`; surfaces badge OK/ERR |
| `monitor` | pyserial reader @115200 | `--seconds`/`--until` bounded mode for agents; exclusive port ownership |
| port handling | auto-detect or `--port` | Clear message when busy/inaccessible (FR-036) |

**Relationships**: invokes `vendor/cdc-badge-os/tools/upload.py`; consumed by *Editor tasks* and by the *Assistant Skill*. Fallback when no host USB: *Webflasher* (in `vendor/cdc-badge-plugins/webflasher`, also hosted upstream).

## 6. Assistant Skill (`cdc-badge-plugin-dev`)

| Field | Description | Validation |
|-------|-------------|------------|
| `SKILL.md` | Canonical skill (frontmatter `name`+`description`) | One source; mirrored to `.claude/skills/` and `.agents/skills/` |
| covered phases | develop / flash / debug | Each phase gives correct, real commands (FR-015) |
| encoded pitfalls | CP437 display, action-callback idx vs user_data, GPIO block list, host_api single source, `wbuf_ok`, canvas back-key | Cited from vendored docs/memory, not hard-coded (FR-016, FR-018) |
| TDD guidance | test-first via `badge test` | Write failing host test, implement, keep green (FR-041) |
| code quality | DRY/KISS/SRP, naming, no magic numbers, error handling | Applies `code-quality.md`; guards against AI pitfalls (FR-042, FR-044) |
| beginner comments | heavy what-comments | Generated code explains each step for learners (FR-043) |
| availability | Claude Code, Codex, opencode | Reachable in all three (FR-017) |

## 7. Agent & SDD Surface

| Field | Description | Validation |
|-------|-------------|------------|
| `AGENTS.md` | Shared agent instructions | Read by Codex + opencode; cross-tool-portable single source (FR-034) |
| `CLAUDE.md` | Claude pointer | Thin; points to `AGENTS.md` |
| `.specify/` | SDD scaffolding | constitution + templates + scripts + `init-options.json` (`feature_numbering: sequential`) committed |
| per-agent SDD commands | `speckit-*` | Committed for Claude (`.claude/skills`) + Codex (`.agents/skills`); opencode reads them; usable on first open with **no interactive agent pick** (multi-agent rollout via committing, not `specify init`); bootstrap MAY auto-detect installed agent CLIs to refresh (FR-019–FR-021) |
| tasks-template default | tests ON | SDD-generated plugin features include automated tests by default (FR-041) |

## 8. CI Pipelines

| Field | Description | Validation |
|-------|-------------|------------|
| `.github/workflows/publish-image.yml` | GitHub → Docker Hub | Login via `DOCKERHUB_*` secrets; build+push `:latest`+`:<sha>`; runs only when secrets present |
| `.gitlab-ci.yml` | GitLab → registry.krim.dev | `docker buildx --push` to `$HARBOR_HOST`; optional cosign sign; default-branch rule; runs only when `HARBOR_*` vars present |

**Relationships**: both build the *Dockerfile* image consumed by the *container alternative*; credential-gated so forks/local use are unaffected (FR-037).

## 9. README Entry Point

| Field | Rule |
|-------|------|
| first section | purpose + both audiences (classical, vibe/SDD) + single first action (FR-028, FR-031) |
| routes | classical (agent-free) and vibe/SDD (names the 3 agents) (FR-029) |
| official links | firmware repo + docs site, plugin repo, host-API reference, web installer, Spec Kit + agent docs (FR-030) |
| beginner walkthrough link | `EASY.md` and `code-quality.md` linked as starting points (FR-038, FR-042) |

## 10. Code-quality guide (`code-quality.md`)

| Field | Rule |
|-------|------|
| standards | DRY, KISS, single responsibility, consistent structure + naming, small focused functions, no magic numbers, robust error handling (FR-042) |
| comment policy | production norm (why-over-what, self-documenting) PLUS the beginner teaching exception (heavy what-comments in skill output), with a note to trend toward production as the student grows (FR-043) |
| AI-agent mistakes section | hallucinated/outdated APIs and packages, security vulnerabilities, overcomplication, ignoring existing patterns / the canonical host API, silent scope creep, unverified/fabricated tests — each with a concrete guard (FR-044) |
| relationships | the skill applies it; README and EASY.md link to it |
