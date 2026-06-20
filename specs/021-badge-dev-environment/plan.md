# Implementation Plan: CDC Badge Development Onboarding Repository (`cdc-badge-development`)

**Branch**: `021-badge-dev-environment` | **Date**: 2026-06-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/021-badge-dev-environment/spec.md`

## Summary

Build a new, standalone repository (`cdc-badge-development`) that makes "clone → build a Rust WASM plugin → flash it onto a CDC Badge → read its logs" trivial for newcomers, and equally serves classical, vibe-coding, and spec-driven-development (SDD) workflows. The repo contains no new firmware; it vendors the firmware and plugin projects as git submodules (updatable knowledge with `host_api.h` as the read-only single source of truth), provisions the full plugin toolchain host-natively (isolated via a Python venv + pinned rustup toolchain) with a development container as the reproducible alternative, ships a single automated `badge` CLI for build/flash/monitor that an AI agent can drive directly, includes a complete cross-tool plugin-development assistant skill (one `SKILL.md` serving Claude Code, Codex, and opencode), pre-installs and activates Spec Kit for all three agents, and publishes the dev-container image through two CI pipelines (GitHub Actions → Docker Hub, GitLab CI → registry.krim.dev).

## Output Target (CRITICAL)

The specification, plan, and tasks are **authored here** in `cdc-badge-os` (`specs/021-badge-dev-environment/`), but **all implementation output lands exclusively in a new, separate repository**. Nothing in this section is written into `cdc-badge-os` except these `specs/021-*` documents.

- **New repository**: `cdc-badge-development`, created locally at `~/GIT/cdc-badge-development`, initialised as its own git repository (not a branch, subdir, or worktree of `cdc-badge-os`).
- **Remotes to create** (because there are two CI pipelines):
  - **GitHub**: `krim404/cdc-badge-development` (drives the Docker Hub publish workflow).
  - **GitLab**: `library/cdc-badge-development` (drives the `registry.krim.dev` pipeline; the Harbor project is `library`, so the image path is `registry.krim.dev/library/cdc-badge-development`).
- **Repo metadata** must be set when the remotes are created: a clear repository **description/about**, topics/tags, and the README as the landing page (FR-028/FR-031 framing).
- **Repository creation is the first implementation step** (the first tasks in `tasks.md`): create the local repo, create both remotes with description/metadata, add the two submodules under `vendor/`, then scaffold everything in the Project Structure below.
- The two vendored submodules point at the **existing** `cdc-badge-os` and `cdc-badge-plugins` upstreams; the new repo never contains firmware source itself.

## Technical Context

**Language/Version**: Rust (toolchain pinned by `vendor/cdc-badge-plugins/rust-toolchain.toml` — stable channel, `wasm32-unknown-unknown` target, `rustfmt`+`clippy`); Python 3.11+ (host tooling + the cross-platform bootstrap `setup.py`, in a venv; **no authored PowerShell** — at most a one-line `.cmd` delegating to Python); a thin Bash wrapper for Unix convenience; YAML (CI); Dockerfile (container alternative).

**Primary Dependencies**: rustup + `wasm32-unknown-unknown`; Binaryen `wasm-opt` (pinned to the version used by the plugins CI, v119 at time of writing); `pyserial>=3.5` (upload/serial); the two git submodules (`cdc-badge-os`, `cdc-badge-plugins`); Spec Kit scaffolding (committed); VS Code (devcontainer + `tasks.json` + recommended extensions). Upstream tools reused, not reimplemented: `vendor/cdc-badge-os/tools/upload.py`, `vendor/cdc-badge-plugins/sdk/plugin_template_rust`, `vendor/cdc-badge-plugins/webflasher`.

**Storage**: N/A — repository is files only. On-device persistence (badge `plugins` partition) belongs to the firmware and is out of scope.

**Testing**: Test-Driven Development is the default for plugin features — host-side `cargo test` for plugin logic, exposed via `badge test`, with the committed SDD scaffolding defaulting test tasks ON (mirroring the upstream host-test culture). End-to-end validation via `quickstart.md` (bootstrap → build → test → flash → live logs). The cross-platform verification CI runs `badge build` + `badge test` on Windows/macOS/Linux.

**Target Platform**: Local developer workstation (macOS / Linux / Windows) running local VS Code; the CDC Badge (ESP32-S3) as the runtime device over USB-CDC at 115200; Docker for the container alternative. Browser/cloud editors (Codespaces) are out of scope.

**Project Type**: Onboarding / tooling meta-repository (not an application or library). It orchestrates submodules, scripts, agent assets, and CI.

**Performance Goals**: Trivial onboarding — clean machine to a built example in ≤3 student actions; flashing is one command; serial logs stream live. No runtime throughput targets (no long-running service).

**Constraints**: No firmware (C++/ESP-IDF) build. Host-native primary path so the agent/tooling get direct USB access; webflasher is the no-USB fallback. Local-only (no Codespaces). `host_api.h` consumed read-only from the submodule, never forked. Repo-authored docs are current-state only. CI pipelines are credential-gated and must not affect local use. **Windows is a first-class, must-be-correct target the maintainer cannot test**, so it is built only on officially cross-platform tooling and verified by a cross-platform CI matrix (windows/macos/ubuntu); the webflasher is the Windows-safe upload route.

**Scale/Scope**: Course-sized — a handful of example/starter plugins, 3 first-class agents (Claude Code, Codex, opencode), 2 CI publish pipelines, 2 vendored submodules.

**Unresolved**: None. All prior NEEDS CLARIFICATION were resolved in the spec's Clarifications session (firmware out-of-scope; host-native primary; automated flash + direct serial; webflasher fallback; submodule vendoring; two CI registries; local-only).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

The project constitution (`.specify/memory/constitution.md`, v1.0.1) governs the **cdc-badge-os firmware**. This feature delivers a **separate repository** that contains no firmware, so the hardware-specific principles do not apply; the cross-cutting principles do and are honored.

| Principle | Applies? | Status |
|-----------|----------|--------|
| I. Module Isolation & Self-Containment | N/A | No firmware modules in this repo. |
| II. Memory Discipline: PSRAM-First | N/A | No on-device code. |
| III. Security & Sandbox Integrity | Partial | No firmware changes; the skill **teaches** the sandbox rules (GPIO block list, capability/pointer validation) as ground truth, never weakens them. `host_api.h` is consumed read-only. PASS. |
| IV. Simplicity & Surgical Change | Yes | Reuse upstream tooling (upload.py, SDK template, webflasher) instead of reimplementing; minimal `badge` CLI; no speculative features. PASS. |
| V. Versioning Discipline & Pre-1.0 Data Freedom | Yes | `host_api.h` stays canonical in the firmware repo and is only mirrored read-only via submodule (never forked/edited). No version numbers bumped. No migration code (submodule pinning + `--remote` update instead). PASS. |
| Docs "current state only" | Yes | README/skill/knowledge pages describe the working feature set only — no history/rationale/migration. PASS. |

**Gate result: PASS** (no violations; Complexity Tracking not required).

## Project Structure

### Documentation (this feature)

```text
specs/021-badge-dev-environment/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (cli, agent-surface, ci, manifest)
└── checklists/
    └── requirements.md  # Spec quality checklist
```

### Source Code (the new `cdc-badge-development` repository root)

```text
cdc-badge-development/
├── README.md                       # Front door: classical + vibe/SDD routes, official links, framing
├── EASY.md                         # Beginner walkthrough: step-by-step hello-world (agent prompts + badge CLI)
├── code-quality.md                 # Clean-code standards (DRY/KISS/SRP), comment policy (beginner-teaching), AI-agent pitfalls
├── AGENTS.md                       # Shared agent instructions (read by Codex + opencode)
├── CLAUDE.md                       # Thin pointer to AGENTS.md + Claude-specific notes
├── .gitmodules
├── .gitattributes                  # Enforce LF for scripts; cross-platform line-ending hygiene
├── vendor/                         # Git submodules (auto-initialised by bootstrap / devcontainer)
│   ├── cdc-badge-os/               # Firmware: canonical host_api.h, website docs, specs/, tools/upload.py
│   └── cdc-badge-plugins/          # SDK, examples, plugin_template_rust, webflasher, manifest schema
├── scripts/
│   ├── setup.py                    # Host-native bootstrap (Python, cross-platform, identical on every OS): rustup+target+wasm-opt+venv+submodules
│   └── setup.sh                    # Optional thin bash wrapper for Unix (calls `python3 scripts/setup.py`); NO PowerShell authored
├── tools/
│   └── badge.py                    # Automated CLI: new/build/flash/monitor/list/start/stop/delete (venv)
├── plugins/                        # Student workspace (copy the starter here)
│   └── starter/                    # Ready-to-build copy/extension of the SDK Rust template
├── knowledge/                      # Cross-link hub into vendor/ docs, specs, host-API reference
│   └── index.md
├── .vscode/
│   ├── extensions.json             # rust-analyzer, even-better-toml, serial-monitor, etc.
│   ├── tasks.json                  # Setup / Build / Flash / Monitor tasks → badge CLI
│   └── settings.json
├── .devcontainer/
│   └── devcontainer.json           # Container alternative; image pulled from registry, postCreate inits submodules
├── Dockerfile                      # Dev image (Rust+wasm32+wasm-opt+python venv) built & pushed by CI
├── .claude/skills/
│   ├── cdc-badge-plugin-dev/SKILL.md   # The real plugin-dev assistant (Claude + opencode read this)
│   └── speckit-*/                  # Committed Spec Kit commands for Claude
├── .agents/skills/
│   ├── cdc-badge-plugin-dev/SKILL.md   # Mirror of the skill (Codex + opencode read this)
│   └── speckit-*/                  # Committed Spec Kit commands for Codex
├── .specify/                       # Committed SDD scaffolding (constitution, templates, scripts, memory)
├── .github/workflows/
│   ├── ci.yml                      # Cross-platform verification matrix (windows/macos/ubuntu): bootstrap + build example
│   └── publish-image.yml           # Build + push dev image → Docker Hub
└── .gitlab-ci.yml                  # Build + push dev image → registry.krim.dev (Harbor) + cosign
```

**Structure Decision**: A single onboarding meta-repository. Upstream knowledge and tooling live under `vendor/` as submodules and are reused, never copied or reimplemented. The student-facing surface is: `README.md` (routing), `scripts/setup.*` (one-shot host-native provisioning), `tools/badge.py` (one automated command for build/flash/monitor), `plugins/starter/` (copy-and-go), and the agent assets (`.claude/skills`, `.agents/skills`, `AGENTS.md`, committed `.specify/`). The container alternative (`.devcontainer/` + `Dockerfile`) and its two CI publish pipelines are isolated from the primary host-native path.

## Complexity Tracking

No constitution violations; section intentionally empty.
