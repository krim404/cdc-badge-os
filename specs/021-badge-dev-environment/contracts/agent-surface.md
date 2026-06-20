# Contract: Agent & SDD Surface (Claude Code, Codex, opencode)

One source of truth per asset, made discoverable in each agent's native location. No per-agent duplication of content beyond the file-location mirroring required by the tools.

## Discovery matrix

| Asset | Claude Code reads | Codex reads | opencode reads |
|-------|-------------------|-------------|----------------|
| Shared instructions | `CLAUDE.md` (thin pointer → `AGENTS.md`) | `AGENTS.md` | `AGENTS.md` (falls back to `CLAUDE.md`) |
| Plugin-dev skill | `.claude/skills/cdc-badge-plugin-dev/SKILL.md` | `.agents/skills/cdc-badge-plugin-dev/SKILL.md` | both `.claude/skills` and `.agents/skills` |
| SDD commands (`speckit-*`) | `.claude/skills/speckit-*` | `.agents/skills/speckit-*` (Codex prompts) | committed `.claude`/`.agents` command files |

**Mirroring rule**: the canonical `SKILL.md` is authored once; the second location is kept identical by the bootstrap (symlink on Unix, copy on Windows). A CI/check guard fails if the two diverge (mirrors the firmware repo's `host_api.h` drift-check philosophy).

## `SKILL.md` contract (`cdc-badge-plugin-dev`)

- **Frontmatter**: `name` (kebab-case), `description` (one line, triggers on plugin develop/build/flash/upload/debug intents).
- **Body covers three phases** (FR-015):
  1. **Develop/scaffold**: `badge new`, project layout, manifest fields, lifecycle exports; links into `vendor/cdc-badge-plugins/docs/getting_started.md` and the host-API reference.
  2. **Flash**: drive `badge flash <name> --start` directly; webflasher fallback when no host USB.
  3. **Debug**: read serial logs via `badge monitor`; interpret rejection reasons (capability/manifest/host-API level/size/auth).
- **Encoded pitfalls** (FR-016, cited from vendored docs/memory, not hard-coded):
  - CP437 display rule — never `gfx->print(const char*)` for user text; use `render::printText`/`drawText`.
  - `plugin_on_action(action, idx, user_data)` — bind to `user_data` (item id), not `idx` (screen position).
  - GPIO hard-block list (incl. octal-PSRAM data lines 33–37); capabilities must be declared in `meta.json`.
  - `host_api.h` is the single source of truth (read-only from `vendor/`); `wbuf_ok` the full pointer size.
  - Canvas back-key footgun — a canvas key callback consumes all keys; pop on back.
- **Knowledge sourcing** (FR-018): facts reference `vendor/` docs so they advance with submodule updates instead of going stale.
- **Test-Driven Development** (FR-041): teach test-first — write a failing host-side test (`badge test` → `cargo test`), implement, keep it green; generated features include automated tests by default.
- **Code quality** (FR-042): apply DRY, KISS, single responsibility, clear names, no magic numbers, and error handling per `code-quality.md`.
- **Beginner-oriented output** (FR-043): generated code is heavily commented to explain *what* it does, so a learner can follow each step (teaching exception to the production "why over what" norm).
- **AI-pitfall guards** (FR-044): verify every host-API call against the vendored `host_api.h`, build + run tests before claiming done, prefer existing patterns, keep changes minimal — per the AI-mistakes section of `code-quality.md`.

## `AGENTS.md` contract

- Build/flash/monitor commands (the `badge` CLI), the no-firmware scope, the host-native-primary / webflasher-fallback rule, the "current state only" docs rule, and pointers to the skill and the `knowledge/` hub.
- Cross-tool-portable (FR-034): a single file serves Codex + opencode; `CLAUDE.md` only points here.

## SDD activation (FR-019–FR-021)

- **No interactive agent pick at first run.** `.specify/` (constitution, templates, scripts, `init-options.json` with `feature_numbering: sequential`) and each agent's committed `speckit-*` command set are present on first open, so every supported agent already exposes `specify/plan/tasks/implement` — the student chooses nothing and runs no `specify init`.
- **Per-agent, generated, committed.** Spec Kit 0.10.2 supports multiple integrations in one repo (`specify integration install claude` + `... install codex`). Each generates that agent's own `speckit-*` files, which differ per agent (Codex's set is not identical to Claude's and omits some). They are committed; opencode reads both dirs. Never copy one agent's `speckit-*` into another's dir.
- **TDD default**: the committed Spec Kit tasks-template defaults test tasks **ON** (not optional), so SDD-generated plugin features include automated tests by default (FR-041).
