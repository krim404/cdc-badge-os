---
description: "Task list for cdc-badge-development onboarding repository"
---

# Tasks: CDC Badge Development Onboarding Repository (`cdc-badge-development`)

**Input**: Design documents from `specs/021-badge-dev-environment/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Test-Driven Development is standard here (FR-041). Plugin logic is tested host-side via `cargo test` (exposed as `badge test`); the cross-platform CI runs build + test; the committed SDD scaffolding defaults test tasks ON. There is no separate test framework beyond cargo.

## Path Conventions (CRITICAL)

**All file paths are relative to the NEW repository root** `~/GIT/cdc-badge-development/`, created in T001. **Nothing is written into `cdc-badge-os`** except the `specs/021-*` documents. Upstream code is referenced only through the `vendor/` submodules. No firmware is added. No PowerShell is authored (bootstrap is `python scripts/setup.py`).

---

## Phase 1: Setup (Repository Creation & Skeleton)

**Purpose**: Bring the new repository and its two remotes into existence with metadata, vendor the upstreams, lay down the skeleton.

- [X] T001 Create the new local git repository at `~/GIT/cdc-badge-development` (`git init`, default branch `main`); the only filesystem location written outside `cdc-badge-os`
- [X] T002 Create the GitHub remote `krim404/cdc-badge-development` with a clear description and topics (`gh repo create`), add it as remote `github`
- [X] T003 Create the GitLab remote `library/cdc-badge-development` with a description, add it as remote `gitlab`
- [X] T004 [P] Add git submodules `vendor/cdc-badge-os` and `vendor/cdc-badge-plugins`, commit `.gitmodules`
- [X] T005 [P] Add `.gitignore` (`/target`, `/.venv`, `/dist`) and `.gitattributes` (force `LF` on `*.sh`, `*.py`, `*.md`)
- [X] T006 [P] Create the directory skeleton: `scripts/`, `tools/`, `plugins/`, `knowledge/`, `.vscode/`, `.devcontainer/`, `.claude/skills/`, `.agents/skills/`, `.specify/`, `.github/workflows/`
- [X] T007 [P] Add `LICENSE` (GPL-3.0, matching upstream) and a placeholder `README.md` (replaced in T036)

---

## Phase 2: Foundational (Toolchain Bootstrap + CLI/Agent Base)

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T008 Implement `scripts/setup.py` — single cross-platform Python bootstrap: install rustup + pinned toolchain + `wasm32-unknown-unknown`, download pinned Binaryen `wasm-opt` for the host OS, create `.venv` + install `pyserial>=3.5`, run `git submodule update --init --recursive`; idempotent; clear non-zero failures; NO PowerShell
- [X] T009 [P] Add `scripts/setup.sh` — thin bash wrapper calling `python3 scripts/setup.py` (Unix convenience only)
- [X] T010 [P] Create `tools/badge.py` — CLI skeleton (argparse dispatch for `new/build/test/flash/monitor/list/start/stop/delete`, venv entry, serial-port auto-detect for `/dev/cu.usbmodem*`/`/dev/ttyACM*`/`/dev/ttyUSB*`/`COM*`); subcommand bodies filled in their stories
- [X] T011 [P] Add `.vscode/extensions.json` (rust-analyzer, even-better-toml, a serial monitor) and `.vscode/settings.json`
- [X] T012 [P] Add `AGENTS.md` (scope, the `badge` CLI, host-native/webflasher rule, current-state docs rule) and `CLAUDE.md` (thin pointer to `AGENTS.md`)
- [X] T013 Commit the scaffold and push to both remotes; confirm `github` and `gitlab` are reachable

**Checkpoint**: `python scripts/setup.py` provisions the toolchain; `badge --help` runs.

---

## Phase 3: User Story 1 - Zero-to-working build + test (Priority: P1) 🎯 MVP

**Goal**: Open → one setup action → build (and test) an example plugin — no manual per-dependency installation.

**Independent Test**: On a clean machine, `python scripts/setup.py` then `badge build`/`badge test` an example succeed.

- [X] T014 [US1] Implement `badge new <name>` in `tools/badge.py` (copy `vendor/cdc-badge-plugins/sdk/plugin_template_rust` → `plugins/<name>/`, rename crate + `meta.json` `id`)
- [X] T015 [US1] Implement `badge build <name>` in `tools/badge.py` (`cargo build --release --target wasm32-unknown-unknown -p <name>` → `wasm-opt -Oz --enable-bulk-memory --enable-nontrapping-float-to-int`; print artifact path)
- [X] T016 [US1] Implement `badge test <name>` in `tools/badge.py` (`cargo test -p <name>` on the host/native target) — the TDD loop (FR-041)
- [X] T017 [P] [US1] Add `plugins/starter/` — a ready-to-build plugin (`Cargo.toml`, `src/lib.rs`, `meta.json`) derived from the vendored template, including a passing host-side unit test that demonstrates TDD and heavily commented for beginners (FR-043)
- [X] T018 [P] [US1] Add `.vscode/tasks.json` `Setup` (`python scripts/setup.py`), `Build plugin` (`badge build`), and `Test plugin` (`badge test`) tasks
- [X] T019 [US1] Add `.github/workflows/ci.yml` — cross-platform verification matrix (`windows-latest`, `macos-latest`, `ubuntu-latest`): checkout+submodules → `python scripts/setup.py` → `badge build starter` → `badge test starter`; push and confirm green on all three OSes (FR-040, SC-013)

**Checkpoint**: example builds AND tests locally, and the CI matrix is green on Windows/macOS/Linux (the maintainer's Windows test substitute).

---

## Phase 4: User Story 2 - Build, flash, run, debug on the badge (Priority: P1)

**Goal**: One automated command builds+uploads+starts a plugin on a connected badge and streams its logs; webflasher is the no-USB fallback.

**Independent Test**: With a badge connected, `badge flash starter --start --monitor` runs the plugin and shows its live log output.

- [X] T020 [US2] Implement `badge flash <name>` in `tools/badge.py` (build-if-stale, then `vendor/cdc-badge-os/tools/upload.py --wasm … --meta … [--lang …] [--pin …]`; optional `--start`; surface `OK`/`ERR`) (FR-009)
- [X] T021 [US2] Implement `badge monitor` in `tools/badge.py` (pyserial @115200; `--seconds`/`--until` bounded mode for agents; exclusive port; clear busy/inaccessible message) (FR-011, FR-035, FR-036)
- [X] T022 [P] [US2] Implement `badge list` / `start` / `stop` / `delete` (PLUGIN LIST/START/STOP/DELETE over serial) (FR-010)
- [X] T023 [P] [US2] Add `.vscode/tasks.json` `Flash plugin` (`badge flash --start --monitor`) and `Monitor serial` tasks
- [X] T024 [US2] Implement PIN authentication (`AUTH <pin>`) in flash/serial paths, with `--pin`/env support (FR-013)
- [X] T025 [P] [US2] Document the WebSerial webflasher fallback (point to `vendor/cdc-badge-plugins/webflasher` + hosted page) for hosts without direct USB access (FR-012)

**Checkpoint**: flash + run + live logs work on a connected badge; webflasher fallback documented.

---

## Phase 5: User Story 3 - AI assistant skill (Priority: P2)

**Goal**: One complete plugin-dev skill (develop/test/flash/debug) usable in Claude Code, Codex, opencode, encoding real pitfalls, TDD, code quality, beginner comments, and AI-mistake guards.

**Independent Test**: In each agent, the skill triggers and gives correct, real, clean, well-commented guidance for develop, flash, and debug.

- [X] T026 [US3] Author `.claude/skills/cdc-badge-plugin-dev/SKILL.md` (frontmatter + phases: develop via `badge new`, test via `badge test`, flash via `badge flash`, debug via `badge monitor`; cite `vendor/` docs) (FR-014, FR-015)
- [X] T027 [US3] Encode the known pitfalls: CP437/`printText`, `plugin_on_action` idx-vs-user_data, GPIO hard-block list (incl. octal-PSRAM 33-37), `host_api.h` single source, `wbuf_ok` full-size validation, canvas back-key — referencing vendored docs (FR-016, FR-018)
- [X] T028 [US3] Encode the working practices in the skill: TDD test-first (`badge test`), code-quality standards from `code-quality.md`, **beginner-oriented heavy comments** in generated code, and AI-pitfall guards (verify host-API calls against vendored `host_api.h`, build + test before claiming done, prefer existing patterns, keep changes minimal) (FR-041, FR-042, FR-043, FR-044)
- [X] T029 [US3] Mirror the skill to `.agents/skills/cdc-badge-plugin-dev/SKILL.md` and wire `scripts/setup.py` to keep the two in sync (symlink Unix / copy Windows) + a drift check (FR-017, FR-034)
- [X] T030 [P] [US3] Extend `AGENTS.md` to point all agents at the skill, `knowledge/`, and `code-quality.md`

**Checkpoint**: the skill is reachable and correct in all three agents, applying TDD + code quality + beginner comments.

---

## Phase 6: User Story 4 - Spec-driven development active out of the box (Priority: P2)

**Goal**: Spec Kit commands present + usable on first open in all three agents, no `specify init`, no interactive pick, with tests defaulted ON.

**Independent Test**: In each agent on a fresh open, list workflow commands and start a spec — it works with no install step; generated tasks include tests.

- [X] T031 [US4] Commit `.specify/` scaffolding: a constitution adapted to this onboarding repo, the templates with the **tasks-template defaulting test tasks ON** (TDD standard, FR-041), the scripts, and `init-options.json` (`feature_numbering: sequential`) (FR-019)
- [X] T032 [US4] Generate and commit each agent's Spec Kit command set: Claude (`.claude/skills/speckit-*`), Codex (`.agents/skills/speckit-*` / prompts); confirm opencode reads the committed files — no student `specify init`, no interactive pick (FR-020, FR-021, research R6)
- [ ] T033 [P] [US4] Add optional agent-CLI auto-detect to `scripts/setup.py` that refreshes the detected agent's command set only when `specify`/`uv` is present (no prompt; falls back to committed files)

**Checkpoint**: SDD works on first open in all three agents with zero choices; generated features include tests.

---

## Phase 7: User Story 5 - Current, cross-linked knowledge (Priority: P3)

**Goal**: Local, cross-linked, updatable knowledge sourced from the submodules.

**Independent Test**: `knowledge/index.md` links resolve; the update action advances the pin.

- [X] T034 [US5] Create `knowledge/index.md` — cross-link hub into `vendor/cdc-badge-os` (host-API reference, `website/.../docs/dev/*`, `specs/`) and `vendor/cdc-badge-plugins` (SDK, examples, `docs/manifest_schema.md`, serial commands) (FR-023, FR-026)
- [X] T035 [P] [US5] Document the single update action (`git submodule update --remote vendor/<name>` + commit) and the read-only `host_api.h` pinning policy (FR-024, FR-027)

**Checkpoint**: knowledge reachable, cross-linked, updatable in one command.

---

## Phase 8: User Story 6 - README + EASY.md + code-quality.md (Priority: P3)

**Goal**: A front door that routes both audiences, a from-zero beginner walkthrough, and the code-quality guide.

**Independent Test**: A first-time reader finds their route from the README's first screen; an absolute beginner follows EASY.md to a running hello-world.

- [X] T036 [US6] Write `README.md`: purpose + both audiences + single first action; two labelled routes (agent-free; vibe/SDD naming the three agents); official upstream links (firmware repo + docs site, plugin repo, host-API reference, web installer, Spec Kit + agent docs); explicit framing; link to `EASY.md` and `code-quality.md` (FR-028–FR-031)
- [X] T037 [US6] Write `EASY.md` — absolute-beginner walkthrough from a bare computer: install VS Code + git + Python per OS (macOS `brew`, Linux `apt`, Windows `winget`/download) → get the repo → `python scripts/setup.py` → hello-world via the exact agent prompts (Claude Code / Codex / opencode) AND the equivalent `badge` CLI commands; note the webflasher as the Windows-safe upload; note features come with tests by default (FR-038, SC-012)
- [X] T038 [US6] Write `code-quality.md` — the standards the skill applies and the student should follow (DRY, KISS, single responsibility, consistent structure + naming, small focused functions, no magic numbers, error handling); the comment policy (production why-over-what + the beginner teaching exception of heavy what-comments); and a "common AI coding-agent mistakes" section (hallucinated/outdated APIs+packages, security vulnerabilities, overcomplication, ignoring existing patterns/the canonical host API, scope creep, unverified/fabricated tests) each with a concrete guard (FR-042, FR-043, FR-044)

**Checkpoint**: a newcomer goes README → EASY → running hello-world; code-quality.md explains the standards and AI pitfalls.

---

## Phase 9: Container Alternative & Image Publishing (Cross-Cutting)

- [X] T039 [P] Add `Dockerfile` — dev image (Rust + `wasm32-unknown-unknown` + pinned `wasm-opt` + Python venv with `pyserial`) with version pins matching the host-native bootstrap (FR-004, FR-005)
- [X] T040 [P] Add `.devcontainer/devcontainer.json` — pulls/builds the image; `postCreateCommand` runs submodule init; no host-USB assumption (FR-004)
- [X] T041 [P] Add `.github/workflows/publish-image.yml` — build + push to Docker Hub `krim404/cdc-badge-development` (`:latest` + `:<sha>`); runs only when `DOCKERHUB_*` secrets are configured (FR-037)
- [X] T042 [P] Add `.gitlab-ci.yml` — `docker buildx` build + push to `registry.krim.dev/library/cdc-badge-development` with optional cosign sign, default-branch rule; runs only when `HARBOR_*` vars are configured; modelled on `~/GIT/selkies-gpu/.gitlab-ci.yml` (FR-037)

**Checkpoint**: the image builds; each pipeline publishes when its credentials are present and no-ops otherwise.

---

## Phase 10: Polish & Validation (Cross-Cutting)

- [X] T043 [P] Run the `quickstart.md` scenarios QS0–QS9 end to end and record results
- [X] T044 [P] Verify the skill↔mirror drift check and submodule auto-init in both `scripts/setup.py` and the devcontainer `postCreate`
- [X] T045 Confirm: no PowerShell is authored anywhere; the CI matrix is green (build + test) on all three OSes; the classical-only loop works with no agent (SC-007); skill output is clean and beginner-commented per `code-quality.md`
- [X] T046 Final commit and push to both remotes; confirm the repository description/topics are set on GitHub and GitLab

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: T001 first; T002/T003 (remotes) and T004–T007 after T001.
- **Foundational (Phase 2)**: depends on Phase 1; T008 (bootstrap) + T010 (CLI skeleton) block the story phases.
- **User Stories (Phase 3-8)**: depend on Phase 2. US1 is the MVP; US2 depends on US1 (`flash` builds via `build`). US3–US6 depend only on Phase 2 and are otherwise independent (US6's EASY.md references US1–US3 outputs).
- **Container & Publishing (Phase 9)**: depends on Phase 1 (remotes) + Phase 2 pins; independent of the host-native stories.
- **Polish (Phase 10)**: depends on all desired stories.

### User Story Dependencies

- **US1 (P1)**: after Foundational. True MVP (build + test + cross-platform CI).
- **US2 (P1)**: after US1.
- **US3 (P2)**: after Foundational; references US1/US2 CLI in its guidance, authored independently. Depends on `code-quality.md` (T038) for its standards — author T038 before/with T028 or reference it.
- **US4 (P2)**: after Foundational; independent.
- **US5 (P3)**: after Foundational (needs Phase 1 submodules); independent.
- **US6 (P3)**: README independent; EASY.md after US1–US3 exist; code-quality.md can be authored early (US3 references it).

### Parallel Opportunities

- Phase 1: T004–T007 parallel after T001.
- Phase 2: T009–T012 parallel after T008.
- US1: T017, T018 parallel; US2: T022, T023, T025 parallel; US3: T030 parallel; US4: T033 parallel; US5: T035 parallel.
- Phase 9: T039–T042 all parallel.
- After Foundational, US3/US4/US5 can run parallel to the US1→US2 chain.

---

## Parallel Example: User Story 1

```bash
# After badge new/build/test land, these are parallel:
Task: "Add plugins/starter/ with a passing host test, beginner-commented"   # T017
Task: "Add Setup/Build/Test VS Code tasks"                                  # T018
```

---

## Implementation Strategy

### MVP First (Phases 1-2 + US1)

1. Phase 1: create repo + remotes + submodules + skeleton.
2. Phase 2: bootstrap + CLI skeleton + agent base.
3. US1: `badge new`/`build`/`test`, starter plugin (with a test, beginner-commented), editor tasks, **cross-platform CI running build + test** (Windows verified from day one).
4. **STOP and VALIDATE**: clean-machine build + test + green CI matrix. The demonstrable MVP.

### Incremental Delivery

1. MVP → green build + test everywhere.
2. US2 → flash/run/logs on hardware.
3. US3 → AI skill (TDD, code quality, beginner comments, AI-pitfall guards) across three agents.
4. US4 → SDD active out of the box with tests defaulted ON.
5. US5 → knowledge hub. US6 → README + EASY.md + code-quality.md.
6. Phase 9 → container + image publishing. Phase 10 → validate.

### Notes

- [P] = different files, no dependency on an incomplete task.
- [US#] maps a task to its story; Setup/Foundational/Container/Polish carry no story label.
- Stand up the cross-platform CI (T019) at US1 so the untestable Windows path is continuously verified.
- Author `code-quality.md` (T038) early if US3's skill (T028) needs to reference it.
- Commit after each task or logical group; push to both remotes.
- Everything lands in `~/GIT/cdc-badge-development`; nothing but `specs/021-*` is written into `cdc-badge-os`.
