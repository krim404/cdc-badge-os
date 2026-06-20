# Phase 0 Research: `cdc-badge-development`

All spec-level NEEDS CLARIFICATION were resolved during the spec's Clarifications session. This document records the technical decisions (with rationale and rejected alternatives) needed to design the repository.

## R1 — Primary onboarding: host-native bootstrap, container as alternative

- **Decision**: The primary path is a single cross-platform Python bootstrap (`scripts/setup.py`, invoked identically on every OS; an optional thin `scripts/setup.sh` bash wrapper for Unix; **no authored PowerShell**) that installs rustup with the pinned toolchain + `wasm32-unknown-unknown`, downloads pinned Binaryen `wasm-opt`, creates a Python venv with `pyserial`, and initialises the submodules. A development container (`.devcontainer/` + `Dockerfile`) is provided as the reproducible/isolated alternative.
- **Rationale**: The agent and tooling must reach the badge's USB port directly for automated flashing and live serial logs; USB passthrough into a container is unreliable on macOS/Windows. Host-native gives direct access; a Python venv + per-user rustup keep the host clean (the user explicitly accepted this: "venv und co sollten das ja genug isolieren können"). Students already run VS Code locally.
- **Alternatives rejected**: Container-primary (blocks direct USB on macOS/Windows); mise/devbox (extra tool to learn, weaker VS Code "open and go"); Codespaces (no USB at all, ruled out in spec).

## R2 — Automated flash: a single `badge` Python CLI

- **Decision**: A single Python CLI (`tools/badge.py`, run from the venv; optionally exposed as a `badge` console script and as VS Code tasks) wraps the whole loop: `new`, `build`, `flash`, `monitor`, `list`, `start`, `stop`, `delete`. `build` runs `cargo build --release --target wasm32-unknown-unknown -p <id>` then `wasm-opt -Oz --enable-bulk-memory --enable-nontrapping-float-to-int`. `flash` builds if needed, then calls the upstream `vendor/cdc-badge-os/tools/upload.py --wasm … --meta … [--lang …] [--pin …]`, optionally `--start`.
- **Rationale**: Python is already required (pyserial) and is the most portable, agent-friendly single entry point across macOS/Linux/Windows. Reusing `upload.py` honors "reuse upstream tooling" and the PLUGIN UPLOAD/UPLOAD_META/UPLOAD_LANG serial protocol (CRC32, READY/OK handshake) instead of reimplementing it.
- **Alternatives rejected**: Makefile (no native Windows), justfile (extra install), raw multi-step `cargo`+`wasm-opt`+`upload.py` (not "trivial", not one agent-invokable action).

## R3 — Live serial logs readable by the agent

- **Decision**: `badge monitor` opens the badge port with pyserial at 115200 and streams decoded lines to stdout. It supports non-interactive use by the agent: a `--seconds N` / `--until <marker>` mode that reads for a bounded time then exits, so an agent can capture logs deterministically.
- **Rationale**: PlatformIO's `pio device monitor` is unavailable in a plugin-only environment (no ESP-IDF). A small pyserial reader is sufficient, OS-portable, and scriptable for an agent (FR-011, FR-035, SC-010).
- **Alternatives rejected**: `pio device monitor` (requires the heavy firmware toolchain); VS Code Serial Monitor extension only (not agent-invokable from a shell).

## R4 — Serial port contention between flash and monitor

- **Decision**: The `badge` CLI owns the port for one operation at a time. `flash` and `monitor` acquire the port exclusively; if the port is busy it reports a clear message and (for the editor task) instructs closing an open monitor first. A combined `badge flash --monitor` releases-then-reopens so the common "flash then watch" loop is one command.
- **Rationale**: A single USB-CDC endpoint cannot be held by two readers; this is the most common beginner failure. Explicit ownership + a clear message satisfies FR-036.
- **Alternatives rejected**: Silent ret/retry loops (confusing); multiplexing the port (overengineered).

## R5 — One skill, three agents (Claude Code, Codex, opencode)

- **Decision**: Author one canonical `SKILL.md` (`cdc-badge-plugin-dev`) and make it discoverable in two committed locations: `.claude/skills/cdc-badge-plugin-dev/SKILL.md` and `.agents/skills/cdc-badge-plugin-dev/SKILL.md`. `AGENTS.md` carries the shared repo instructions; `CLAUDE.md` is a thin pointer to `AGENTS.md`.
- **Rationale**: Claude Code reads `.claude/skills`; Codex reads `.agents/skills`; opencode reads **both** `.claude/skills` and `.agents/skills` plus `AGENTS.md`. Committing the same `SKILL.md` to the two dirs (kept in sync by the bootstrap — symlink on Unix, copy on Windows) means one source of truth serves all three with no per-agent drift (FR-014, FR-017, FR-034).
- **Skill content** is harvested from real on-machine material: the firmware `website/src/content/docs/dev/*` (plugin-sdk, host-api, proto/serial-commands), the plugin SDK docs (`getting_started.md`, `manifest_schema.md`), and the accumulated developer pitfalls (CP437/`printText` display rule; the `plugin_on_action` idx=screen-position vs user_data=item-id contract; GPIO hard-block list incl. octal-PSRAM 33-37; `host_api.h` single source of truth; `wbuf_ok` full-size pointer validation; canvas back-key footgun). The skill references the vendored docs (FR-016, FR-018) rather than hard-coding values.
- **Alternatives rejected**: Per-agent duplicated skills (drift); a single agent only (violates FR-017).

## R6 — Spec Kit pre-installed and active for all three agents (no first-run pick)

- **Question raised**: `specify init` asks which agent to set up. Is multi-agent rollout possible without a first-run pick, and are the per-agent command files even the same?
- **Answer**: Yes to multi-agent, and **no, the command files are not the same**. Spec Kit 0.10.2 supports **multiple integrations in one repo** (`specify integration install <agent>` / `use`). Each integration **generates that agent's own** `speckit-*` command files, and they genuinely differ per agent (Claude's `.claude/skills/speckit-*` are not byte-identical to Codex's `.agents/skills/speckit-*`; Codex even omits some, e.g. `speckit-agent-context-update`). `claude` and `codex` are multi-install-safe and coexist; `opencode` reads both `.claude/skills` and `.agents/skills` (+ `AGENTS.md`), so it needs no separate install.
- **Decision**: Commit the shared `.specify/` scaffolding once, plus each agent's **generated** command set - install the `claude` and `codex` integrations so the per-agent `speckit-*` files are correct, and let opencode read them. **Never copy one agent's `speckit-*` into another agent's dir** (wrong frontmatter/commands). The custom plugin-dev skill is a single generic `SKILL.md` mirrored to `.claude/skills` and `.agents/skills`; `setup.py` keeps **only that one** in sync and never touches the per-agent speckit files. Everything is committed, so there is **no interactive agent pick at first run**.
- **Rationale**: per-agent generation is the supported, correct mechanism; the committed generated sets keep SDD "active out of the box" with zero choices (FR-019–FR-021).
- **Generation (maintainer)**: `specify integration install claude` + `... install codex`; `specify integration upgrade` when Spec Kit updates. `init-options.json` records `feature_numbering: sequential`.
- **Alternatives rejected**: copying one agent's `speckit-*` into another agent's dir (produces the wrong per-agent format); a dedicated opencode integration (not multi-install-safe, and unnecessary since opencode reads the committed `.claude`/`.agents` dirs).

## R7 — Knowledge vendored as git submodules

- **Decision**: `vendor/cdc-badge-os` and `vendor/cdc-badge-plugins` as git submodules, pinned to a commit. The bootstrap and the devcontainer `postCreate` run `git submodule update --init --recursive` automatically. Updating to a newer upstream is one documented command (`git submodule update --remote vendor/<name>` + commit). `host_api.h`, the SDK, examples, manifest schema, serial-command docs, and the firmware `specs/` are all read from the submodules; cross-links point into `vendor/`.
- **Rationale**: Matches the user's "git sub" suggestion and the "works for future versions" requirement; keeps `host_api.h` canonical and read-only (Constitution V); students never manage submodules by hand (FR-024, FR-025, FR-027).
- **Alternatives rejected**: `git subtree` (larger repo, heavier updates, easier to accidentally edit the vendored copy); static copies (drift fastest); fetch-script into an ignored dir (not version-pinned in the course repo).

## R8 — Dev-container image published by two CI pipelines

- **Decision**: Two credential-gated pipelines build the same `Dockerfile` and publish the dev image:
  - **GitHub Actions** (`.github/workflows/publish-image.yml`): `docker/login-action` (Docker Hub via `DOCKERHUB_USERNAME`/`DOCKERHUB_TOKEN` secrets) → `docker/build-push-action` with tags `:latest` + `:<git-sha>`/`:<ref>`. Runs only when the secrets are present.
  - **GitLab CI** (`.gitlab-ci.yml`): modelled on `~/GIT/selkies-gpu/.gitlab-ci.yml` — `image: docker:27`, `docker login $HARBOR_HOST` (= `registry.krim.dev`, vars `HARBOR_USERNAME`/`HARBOR_PASSWORD`), `docker buildx build -t $IMAGE:$CI_COMMIT_REF_SLUG -t $IMAGE:latest --metadata-file metadata.json --push .`, then an optional `cosign` sign stage on the digest; rule `if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH`.
- **Rationale**: The user asked for both registries and pointed at selkies-gpu as the GitLab template. Credential-gating keeps forks/local use unaffected (FR-037, SC-011, edge case "registry credentials absent").
- **Open detail (implementation-time)**: exact Docker Hub repo namespace and `registry.krim.dev` project path; pin Binaryen version against the plugins submodule's workflow at implementation time (v119 observed now).
- **Alternatives rejected**: One registry only (user wants both); GitHub Container Registry (user specified Docker Hub).

## R9 — Toolchain version sourcing

- **Decision**: The Rust toolchain is governed by `vendor/cdc-badge-plugins/rust-toolchain.toml` (stable, `wasm32-unknown-unknown`, `rustfmt`+`clippy`). The bootstrap respects it (rustup auto-selects on entering the workspace). `wasm-opt` is pinned to the plugins-CI Binaryen version. `pyserial>=3.5` matches `vendor/cdc-badge-os/tools/requirements.txt`.
- **Rationale**: One pinned source of truth per dependency keeps the host-native path, the container, and CI byte-reproducible (FR-005).
- **Alternatives rejected**: Independently pinning versions in the new repo (would drift from the SDK the students actually build against).

## R10 — Windows correctness without the maintainer being able to test it

- **Decision**: Treat Windows as a first-class, must-be-correct target and guarantee it by construction + automation, not by manual testing:
  1. **Cross-platform tooling only**: rustup (official Windows installer), Python from python.org/winget, `pyserial` (handles `COM*` natively — confirmed in upstream `upload.py`), Binaryen `wasm-opt` Windows binary. The `badge` CLI is pure Python; VS Code tasks invoke the venv Python OS-agnostically; no bash-isms in tasks.
  2. **Verification CI matrix** (`.github/workflows/ci.yml`): runs on `windows-latest`, `macos-latest`, `ubuntu-latest`; executes the bootstrap and builds the example plugin. This is the substitute for the maintainer's inability to test Windows — every change gets a Windows green/red signal. Hardware steps (flash/serial) run their non-hardware portions (arg parsing, build, port enumeration) where possible and are otherwise marked manual.
  3. **Windows-safe upload route**: the browser WebSerial webflasher (Edge/Chrome) avoids COM driver/permission/locking pitfalls entirely; `EASY.md` presents it as the recommended Windows upload step, with `badge flash` (COM via pyserial) as the host-native option.
  4. **No authored PowerShell + documented Windows specifics**: the maintainer does not use PowerShell and cannot test it, so the bootstrap is `python scripts/setup.py` run identically on every OS — there is no `.ps1` to maintain; any unavoidable Windows convenience wrapper is a minimal one-line `.cmd` delegating to Python. The USB-CDC COM driver (usbser, normally automatic), COM-port naming/auto-detection, `.gitattributes` to keep scripts LF, and Docker Desktop/WSL2 (+ `usbipd-win` to pass the serial device into WSL) as the container option are documented.
- **Rationale**: "Most students are on Windows, the maintainer is on macOS and cannot test Windows — it must be perfect." The only credible way to assure an untestable platform is to minimise bespoke OS logic and let CI exercise the real Windows path (FR-039, FR-040, SC-013).
- **Alternatives rejected**: hand-rolled Windows batch logic (untestable, fragile); assuming WSL for everyone (extra setup, not "trivial"); skipping Windows CI and hoping (directly contradicts "it must be perfect").

## R11 — Test-Driven Development as the default

- **Decision**: Plugin logic is unit-tested on the host (`cargo test` on the native target — pure logic is separated from the host-API FFI so it is testable), exposed as `badge test <name>`. The assistant skill teaches test-first (write a failing host test, implement, keep green). The committed `.specify/` tasks-template defaults test tasks **ON** (not optional), and the verification CI runs `badge test` alongside `badge build`.
- **Rationale**: the user wants automated tests to be standard, mirroring the upstream host-test culture. Plugin logic is host-testable, so TDD is practical (FR-041, SC-014).
- **Reconciliation**: the firmware constitution marks tests *optional* because firmware is hardware-verified; the new repo overrides that for plugin logic precisely because that logic IS host-testable, so tests default ON. The skill explains isolating pure logic and mocking/abstracting the host-API boundary so it can be tested off-device.
- **Alternatives rejected**: tests-optional (contradicts the request); on-device-only testing (slow, not a TDD loop).

## R12 — Code-quality standards, comment policy, and AI-agent pitfall guardrails

- **Decision**: Ship a student-facing `code-quality.md` that teaches the established standards — DRY, KISS, single responsibility (SRP), consistent structure and naming, small focused functions, no magic numbers, robust error handling, and comment quality — which the skill applies when generating or reviewing code. Comment policy carries a deliberate **teaching exception**: skill-generated code is heavily commented to explain *what* it does for beginners, unlike the production norm (favour *why over what* + self-documenting code); the guide states this and that students should trend toward the production norm as they grow. A dedicated section lists the **common AI coding-agent mistakes** (hallucinated/outdated APIs and packages, security vulnerabilities, overcomplication and unnecessary abstraction, ignoring existing patterns and the canonical host API, silent scope creep, unverified/fabricated tests) with concrete guards; the skill actively guards (verify every host-API call against the vendored `host_api.h`, build + run tests before claiming done, prefer existing patterns, keep changes minimal).
- **Rationale**: the user wants all important code standards upheld, a student-facing guide, and explicit AI-pitfall awareness. Grounded in established clean-code and comment best practices and 2026 data on AI-generated-code error rates (≈15-20% hallucinated APIs/packages; 29-45% of AI code carries security issues).
- **Reconciliation**: the heavy-comment teaching policy intentionally diverges from the upstream "minimal, current-state-only" comment rule because the audience is learners; this is documented as an explicit exception scoped to this teaching repo.
- **Alternatives rejected**: reuse the firmware's minimal-comment rule (wrong for beginners); ship no guide (students miss the standards); a separate `ai-pitfalls.md` (doc sprawl — folded into `code-quality.md`).

## Sources

- Spec Kit integrations (multiple per repo via `specify integration install`/`use`, verified with 0.10.2): <https://github.github.io/spec-kit/reference/integrations.html>
- opencode skills (reads `.claude/skills` + `.agents/skills` + `AGENTS.md`): <https://opencode.ai/docs/skills/>, <https://opencode.ai/docs/rules/>
- Codex skills (`.agents/skills`): <https://developers.openai.com/codex/skills>
- Container USB-passthrough limitation: <https://github.com/orgs/community/discussions/47619>
- GitLab CI template: `~/GIT/selkies-gpu/.gitlab-ci.yml`
- Ground truth: `vendor/cdc-badge-plugins` (SDK, templates, `docs/getting_started.md`, `docs/manifest_schema.md`, `rust-toolchain.toml`, `webflasher/`), `vendor/cdc-badge-os` (`tools/upload.py`, `tools/requirements.txt`, `website/src/content/docs/dev/*`)
- Clean-code principles: <https://blog.codacy.com/clean-code-principles>, <https://www.pullchecklist.com/posts/clean-coding-principles>
- Comment best practices ("why over what", self-documenting): <https://swimm.io/learn/code-collaboration/comments-in-code-best-practices-and-mistakes-to-avoid>
- AI coding-agent mistakes (hallucinated APIs/packages, security, overcomplication): <https://vocal.media/futurism/8-ai-code-generation-mistakes-devs-must-fix-to-win-2026>, <https://stackoverflow.blog/2026/01/28/are-bugs-and-incidents-inevitable-with-ai-coding-agents/>, <https://arxiv.org/pdf/2511.00776>
