# Feature Specification: CDC Badge Development Onboarding Repository (`cdc-badge-development`)

**Feature Branch**: `021-badge-dev-environment`

**Created**: 2026-06-19

**Status**: Draft

**Input**: User description: "Ein neues Repo `cdc-badge-development` als trivialer Einstieg für einen Kurs, in dem Studenten erstmals Vibe-Coding und Spec-Driven-Development ausprobieren. Es soll (1) ein vollständiges, direkt in VS Code öffenbares Entwicklungssystem mitbringen, (2) alle Tools/Dependencies, um WASM-Plugins in Rust für das Badge zu bauen, zu installieren und zu debuggen - möglichst einfach aus VS Code heraus, (3) einen echten, vollständigen Agent-Skill (kein Template), der bei Entwicklung, Upload und Debugging hilft, sowie Speckit/SDD vorinstalliert und aktiv. Die README muss klar machen, dass es sowohl für klassische als auch für Vibe/SDD-Entwickler ein einfacher Einstieg in Embedded-Software ist, und auf offizielle Ressourcen verlinken. Wissen aus cdc-badge-os und cdc-badge-plugins soll eingebunden und querverlinkt werden (möglichst als aktualisierbare Referenz, z. B. Submodule), damit es auch für kommende Versionen funktioniert. Nicht nur Claude Code, sondern auch opencode und Codex müssen nutzbar sein."

## Overview

`cdc-badge-development` is a self-contained starter repository whose single purpose is to let a newcomer go from "I have nothing installed" to "I built a WebAssembly plugin and ran it on a physical CDC Badge" with the smallest possible number of steps. It is the course environment for students who are trying **vibe coding** and **spec-driven development (SDD)** for the first time, while remaining equally usable by developers who prefer a classical, agent-free workflow.

The repository does not contain new firmware. It packages and points at what already exists: the Rust WASM plugin SDK and examples (`cdc-badge-plugins`), the firmware's developer documentation and feature specifications (`cdc-badge-os`), the upload/debug tooling, a ready-to-run editor environment, a real AI assistant skill for plugin work, and an activated spec-driven-development workflow.

## Clarifications

### Session 2026-06-19

- Q: Is requirement (3) a *template* for a skill, or a complete skill? → A: A complete, working agent skill that assists with plugin development, uploading, and debugging - not a template. Its knowledge is drawn from the existing project material (firmware/SDK docs, accumulated developer memory, existing skills) on this machine.
- Q: Must the skill work only in Claude Code? → A: It must be usable in Claude Code, Codex, and opencode. Do not make it specific to one agent.
- Q: How should Speckit be present? → A: Pre-installed and active out of the box (zero manual install by the student), ideally bootstrapped/activated automatically on first open (a "run first"/init step) so the SDD slash-commands and templates are immediately available.
- Q: Is firmware (C++/ESP-IDF) development in scope? → A: No. Firmware development is explicitly not part of this repository; the firmware's specifications and docs are bundled only as read-only knowledge (confirms FR-032).
- Q: What is the primary trivial-onboarding mechanism? → A: The host-native bootstrap (a single cross-platform Python script `scripts/setup.py`, with an optional thin bash wrapper for Unix and no authored PowerShell, isolated via a Python venv plus a pinned rustup toolchain) is the primary path, so the agent has direct host USB access for flashing and serial logs; a development container is provided as the reproducible/isolated build alternative.
- Q: How is the upstream knowledge vendored so it stays valid for future versions? → A: Git submodules of cdc-badge-os and cdc-badge-plugins, auto-initialized by the onboarding action so students never manage them; one documented command advances them to a newer upstream release.
- Q: Must the trivial path also work in a browser/cloud editor (e.g. Codespaces)? → A: No. Local VS Code is the only target; cloud/Codespaces support is out of scope.
- Q: What is the recommended default flash/upload route? → A: Automated host-side flashing via a single, non-interactive command - with the AI agent able to read serial logs directly on the host - is the primary path; the browser-based WebSerial webflasher is the documented fallback (e.g. when using the container alternative or a host without USB access).
- Q: Should the Dev Container image be published, and how? → A: Yes, via two CI pipelines: a GitHub Actions workflow that builds and pushes the image to Docker Hub, and a GitLab CI pipeline that builds and pushes it to registry.krim.dev (modelled on an existing repo's GitLab CI, e.g. selkies under ~/GIT).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Zero-to-working in one obvious step (Priority: P1)

A student who has never built embedded software clones (or opens) the repository, follows the single most prominent instruction in the README, and within minutes has a working toolchain and a successfully **built** example plugin - without manually researching, choosing, or installing any individual dependency (Rust, WASM target, `wasm-opt`, Python, serial tooling).

**Why this priority**: This is the entire reason the repository exists. If the first run is not trivial, nothing else matters. It is the minimum viable product: a one-action path to a green build.

**Independent Test**: On a clean machine with only an editor installed, follow the README's primary "Start here" instruction (the host-native bootstrap) and confirm an example plugin compiles to a `.wasm` artifact with no additional manual dependency setup.

**Acceptance Scenarios**:

1. **Given** a clean machine and the repository, **When** the student performs the single primary onboarding action described first in the README (the host-native bootstrap), **Then** the complete plugin toolchain is provisioned automatically on the host (no per-dependency manual steps) and the environment reports ready.
2. **Given** a ready environment, **When** the student runs the documented "build the example" action, **Then** an example plugin builds successfully and the location of the resulting artifact is shown.
3. **Given** the repository opened in the editor, **When** the editor finishes loading, **Then** the recommended extensions and ready-to-run tasks for building, flashing, and reading the serial log are offered or pre-configured, so the core actions are discoverable from the editor UI (not only the terminal).
4. **Given** a student who wants a reproducible or isolated build environment, **When** they open the provided development container instead, **Then** the same toolchain is provisioned and the same example builds (uploading then uses the webflasher fallback).

---

### User Story 2 - Build, upload, and run my own plugin on the badge (Priority: P1)

A student creates a new plugin from the provided starting point, builds it, uploads it onto a physically connected CDC Badge, starts it, and sees it run on the device - then reads the badge's log output to confirm behaviour.

**Why this priority**: The course goal is "make embedded software actually run on hardware." Building without being able to get the artifact onto the device, observe it, and read its output would not teach the loop that matters. This is co-equal P1 with Story 1.

**Independent Test**: With a badge connected over USB, take a freshly created plugin from the template, build it, flash it with the single automated command, start it, and confirm it runs on the device and that its log output streams live.

**Acceptance Scenarios**:

1. **Given** the ready environment, **When** the student copies the plugin starting point and renames it, **Then** they have a buildable plugin project with a valid manifest and a minimal working source skeleton.
2. **Given** a connected badge, **When** the student runs the single automated flash command (or the editor's Flash task), **Then** the plugin is built if needed, its module and manifest are transferred, and the badge confirms success - with no manual chunking, CRCs, or device-path fiddling.
3. **Given** an uploaded plugin, **When** the student starts it (from the device menu or via the documented command), **Then** the plugin runs on the device.
4. **Given** a running plugin, **When** the student (or the AI agent) opens the serial monitor or runs the log command on the host, **Then** the plugin's log output and any errors stream live.
5. **Given** a host that cannot access the badge's USB port directly (e.g. the containerised build alternative), **When** the student needs to upload, **Then** the browser-based WebSerial webflasher provides a documented fallback that needs no host USB access.

---

### User Story 3 - AI-assisted plugin development, upload, and debugging (Priority: P2)

A student using an AI coding agent invokes a bundled, ready-to-use assistant skill that guides them through writing a plugin, building it, uploading it, and diagnosing problems. The skill encodes the project's real conventions, commands, and known pitfalls, so the agent gives correct, badge-specific help rather than generic guesses. The same assistance is available whether the student uses Claude Code, Codex, or opencode.

**Why this priority**: "Vibe coding" is an explicit course objective, and an agent that does not know the badge's quirks produces broken plugins. A real, accurate skill turns the agent into a competent pair-programmer for this specific platform. It builds on the working build/upload loop from Stories 1-2.

**Independent Test**: In each of the three target agents, trigger the skill on a plugin task and confirm it provides correct, badge-specific steps for at least one development action, one upload action, and one debugging action, referencing the real commands and constraints (not invented ones).

**Acceptance Scenarios**:

1. **Given** any of the three supported agents open on the repository, **When** the student asks for help building or scaffolding a plugin, **Then** the skill provides the correct, current build and project-layout guidance for this platform.
2. **Given** a built plugin, **When** the student asks the agent to upload it, **Then** the skill drives the automated host-side flash command directly (falling back to the webflasher when the host has no USB access), using the real tooling.
3. **Given** a plugin that misbehaves or is rejected by the badge, **When** the student asks the agent to debug it, **Then** the skill reads the device's serial log directly on the host, interprets capability/manifest/host-API errors, and applies the project's documented pitfalls (for example text-encoding, input-callback, and capability/GPIO constraints).
4. **Given** the same task posed in Claude Code, Codex, and opencode, **When** the skill is invoked, **Then** each agent reaches equivalent guidance (the skill is not exclusive to one agent).
5. **Given** the underlying badge SDK or documentation is updated, **When** the skill's referenced knowledge is reviewed, **Then** the skill draws on the repository's vendored, updatable knowledge sources rather than hard-coded values that silently go stale.
6. **Given** a student builds a plugin feature, **When** the skill guides implementation, **Then** it directs a test-first loop - write automated host-side tests, see them fail, implement, keep them green - so the feature ships with automated tests by default.
7. **Given** the skill generates code, **When** the student reads it, **Then** it follows the `code-quality.md` standards (DRY, KISS, single responsibility, clear names, no magic numbers) and is commented densely enough for a beginner to follow each step.

---

### User Story 4 - Spec-driven development is active out of the box (Priority: P2)

A student opens the repository and immediately practises spec-driven development: the SDD workflow (specify → plan → tasks → implement) and its templates and project principles are already present and active, with no installation step. The student can issue the first SDD command and have it work.

**Why this priority**: SDD is the second explicit course objective. If students had to install and configure the SDD toolkit themselves, the "first time trying SDD" experience would fail before it starts. It is independent of the AI skill (Story 3) and of hardware (Story 2).

**Independent Test**: From a fresh open, issue the first spec-driven-development command in a supported agent and confirm it executes against the repository's pre-provided SDD scaffolding without any manual installation.

**Acceptance Scenarios**:

1. **Given** a freshly opened repository, **When** the student lists available workflow commands in their agent, **Then** the spec-driven-development commands are present and usable without an install step.
2. **Given** the SDD workflow is active, **When** the student starts a new specification, **Then** the provided templates and project principles are applied automatically.
3. **Given** the student switches between the supported agents, **When** they use the SDD workflow, **Then** it is available in each (activated automatically on first open if a per-agent activation is needed).
4. **Given** a student who only wants classical development, **When** they ignore the SDD and AI features, **Then** the build/upload/debug workflow still works unchanged.

---

### User Story 5 - Current, cross-linked knowledge that survives future versions (Priority: P3)

A student (or the agent skill) needs authoritative information about the host API, plugin capabilities, manifest schema, serial commands, or a firmware feature. The repository provides this knowledge locally as a navigable, cross-linked reference, sourced from the firmware and plugin projects, and it stays valid as those projects release new versions without the course material being rewritten by hand.

**Why this priority**: Accurate, discoverable knowledge underpins Stories 2-4, but the build/upload loop can exist before the knowledge base is fully wired. It is the durability requirement that keeps the repo useful across badge/SDK releases.

**Independent Test**: From within the repository, follow links to the host-API reference, the plugin SDK/manifest documentation, and at least one firmware feature specification, and confirm they resolve to current source-of-truth content; then confirm a single documented update action refreshes that content to a newer upstream version.

**Acceptance Scenarios**:

1. **Given** the repository, **When** the student opens the knowledge section, **Then** the plugin SDK, host-API reference, manifest/capability documentation, and the firmware feature specifications are reachable locally and cross-linked.
2. **Given** an upstream release of the firmware or plugin project, **When** the maintainer runs the single documented update action, **Then** the vendored knowledge advances to the newer version without manual copying of files.
3. **Given** a student exploring a capability, **When** they follow a cross-link from a plugin example to the relevant host-API entry or feature spec, **Then** the link resolves within the repository.
4. **Given** the repository is cloned, **When** the student performs the primary onboarding action, **Then** the vendored knowledge sources are present automatically (the student never has to discover or run a separate command to obtain them).

---

### User Story 6 - One README that serves both audiences (Priority: P3)

A visitor reads the README and, within the first screen, understands what the repository is, that it serves both classical and vibe/SDD developers, and exactly what to do first. The README routes each audience to the right path and links out to the official upstream resources.

**Why this priority**: The README is the front door the user explicitly emphasised. It is lower priority only because the underlying capabilities (Stories 1-5) must exist for it to describe; once they do, clear routing is what makes the trivial path actually findable.

**Independent Test**: A first-time reader can identify, from the top of the README, which path to follow for their preferred style and reach the corresponding section without scrolling past unrelated content.

**Acceptance Scenarios**:

1. **Given** the README, **When** a reader opens it, **Then** the first section states the purpose and names both audiences (classical developers and vibe/SDD developers) and the single recommended first action.
2. **Given** the README, **When** a classical developer reads on, **Then** there is a clearly labelled path that does not require any AI agent.
3. **Given** the README, **When** a vibe/SDD developer reads on, **Then** there is a clearly labelled path covering the AI skill and the SDD workflow, naming the supported agents.
4. **Given** the README, **When** a reader wants authoritative detail, **Then** it links to the official upstream resources (firmware repository and docs site, plugin repository, host-API reference, web installer, and the SDD toolkit and agent documentation).

---

### Edge Cases

- **No container runtime installed**: the primary host-native path needs no container; the development-container alternative requires Docker and is optional.
- **No direct USB access** (the containerised build alternative, or a host where the port is unavailable): the primary host-native path flashes and reads serial directly; when direct USB access is missing, the browser WebSerial webflasher is the documented fallback for upload.
- **Serial port contention**: flashing and the serial monitor share the single USB port; the tooling and skill avoid or release a conflicting open monitor before flashing, and surface a clear message when the port is busy or permission-denied (e.g. the Linux dialout group).
- **Container registry credentials absent**: each publish pipeline runs only when its registry credentials are configured; without them the image is not pushed and local (non-CI) use is unaffected.
- **Windows specifics** (majority of students; maintainer cannot test Windows and does not use PowerShell): the bootstrap is `python scripts/setup.py` (no PowerShell); the USB-CDC COM driver and COM-port naming are handled or documented; the browser WebSerial webflasher is the bulletproof Windows upload route; Docker Desktop/WSL2 (with usbipd-win for serial) is the container option; correctness is guarded by the cross-platform verification CI.
- **Badge requires a PIN** for serial operations: the flash/debug instructions and the skill account for the authentication step.
- **Plugin exceeds the device size limit** or **declares a capability/host-API level the firmware does not provide**: the debugging guidance explains the resulting rejection and how to read it.
- **Upstream SDK/host-API drifts** from the vendored copy: there is a single documented action to update the vendored knowledge; the skill references the vendored copy so it does not silently go stale.
- **Student only wants classical development**: every AI/SDD feature is optional and ignorable; the core build/upload/debug loop works without any agent.
- **A future badge/SDK version** changes commands or capabilities: because knowledge is vendored as an updatable reference, advancing it does not require rewriting course material.
- **Pinned versus latest upstream**: the environment is reproducible (pinned), yet a deliberate update path exists; both states are documented.

## Requirements *(mandatory)*

### Functional Requirements

#### Onboarding & environment (Story 1)

- **FR-001**: The repository MUST provide a single, prominently documented primary onboarding action - a host-native bootstrap script - that provisions the complete plugin-development toolchain automatically on the host, with no manual per-dependency installation by the student, so the AI agent and tooling have direct host access to the badge's USB port.
- **FR-002**: The provisioned toolchain MUST include everything required to build a Rust WASM plugin for the badge (Rust with the badge's WASM target, the WASM size-optimisation tool, and the Python-based upload/debug tooling and its dependencies), at versions compatible with the current plugin SDK.
- **FR-003**: The repository MUST be openable directly in VS Code such that, on open, the editor offers or applies the recommended environment (recommended extensions and ready-to-run tasks for building, flashing, and reading the serial log), making the core actions discoverable from the editor UI as well as the terminal.
- **FR-004**: The repository MUST also provide a reproducible development container as an alternative build environment for users wanting isolation or a pinned image, provisioning the same toolchain and building the same example; uploading from the container uses the webflasher fallback.
- **FR-005**: The environment MUST be reproducible: dependency versions are pinned, and the host-native path MUST isolate its dependencies (a Python virtual environment for the tooling and a pinned rustup toolchain) so it does not pollute the student's system, so that two students provisioning at the same time obtain the same working toolchain.
- **FR-006**: Onboarding MUST NOT require the student to make tool-selection decisions or hunt for documentation to reach a first successful build; the default path is opinionated and works unattended.

#### Build, upload, run, debug (Story 2)

- **FR-007**: The repository MUST include a ready-to-copy plugin starting point (project skeleton plus a valid manifest plus a minimal working source) that builds without modification.
- **FR-008**: The repository MUST provide a documented action to build a plugin to its deliverable artifact and to report the artifact's location.
- **FR-009**: The repository MUST provide a single automated flash command that uploads a plugin (module and manifest, and optional translations) onto a connected badge non-interactively - building first if needed - and surfaces the badge's success/failure result, without the student handling chunking, CRCs, or device paths manually.
- **FR-010**: The repository MUST document how to start an uploaded plugin on the device and how to stop, list, inspect, and delete plugins on the device.
- **FR-011**: The repository MUST provide a host-side way to read the badge's serial log output live for debugging (usable from the editor and the terminal) and MUST document how to interpret rejection reasons (capability, manifest, host-API level, size, and authentication errors).
- **FR-012**: The recommended default flash/upload route MUST be the automated host-side flash command running directly on the host; the browser-based WebSerial webflasher MUST be documented as the fallback for when the host cannot access the badge's USB port (e.g. the container alternative). The fallback MUST NOT require USB passthrough into a container.
- **FR-013**: The flash/debug instructions MUST account for a badge that requires authentication (a PIN) for serial operations.
- **FR-035**: The flash and serial-log actions MUST be directly invokable by an AI agent on the host - non-interactive and scriptable - so the agent can flash a plugin and read its logs without human intervention.
- **FR-036**: The flash and serial-monitor tooling MUST handle single-port contention (avoid or release a conflicting open monitor before flashing) and MUST surface a clear message when the serial port is busy or inaccessible.

#### AI assistant skill (Story 3)

- **FR-014**: The repository MUST include a complete, working AI assistant skill (not a template or placeholder) dedicated to badge plugin development.
- **FR-015**: The skill MUST assist with all three phases and be able to act on the host directly: developing/scaffolding a plugin, flashing it via the automated host-side command (with the webflasher as the fallback when the host has no USB access), and debugging it by reading the serial log directly.
- **FR-016**: The skill's guidance MUST reflect the project's real, current conventions and commands and MUST encode the project's known plugin pitfalls (at minimum: text-encoding/display constraints, the input/action-callback contract, capability and GPIO restrictions, the host-API-as-single-source-of-truth rule, and pointer/length validation expectations), so the agent gives correct platform-specific help.
- **FR-017**: The skill MUST be usable across all three target agents (Claude Code, Codex, and opencode) and MUST NOT be implemented in a way that ties it exclusively to one agent; where an agent needs a native form, equivalent guidance MUST be reachable in each.
- **FR-018**: The skill MUST source its factual knowledge from the repository's vendored, updatable knowledge (SDK, host-API reference, docs) rather than from values hard-coded in the skill that would silently diverge from upstream.
- **FR-041**: Test-Driven Development MUST be a standard part of the workflow, mirroring the upstream project's host-test culture: the assistant skill MUST guide writing automated tests first (host-side `cargo test` for plugin logic) and keeping them green; the `badge` CLI MUST provide a `badge test` command that runs them; and the committed SDD scaffolding MUST default to including test tasks (not optional), so features generated via the workflow ship with automated tests by default. The cross-platform verification CI MUST run these tests.
- **FR-042**: The repository MUST include a code-quality guide (`code-quality.md`) explaining the professional standards the skill applies and the student should follow - at minimum DRY, KISS, single responsibility, consistent structure and naming, small focused functions, no magic numbers, robust error handling, and comment quality - and the skill MUST apply these standards when generating or reviewing code.
- **FR-043**: Code generated by the skill MUST be explicitly beginner-oriented: generously commented to explain what the code does (not only why), so a learner can follow the output step by step. `code-quality.md` MUST document this teaching exception and how it differs from production comment norms (favouring "why over what" and self-documenting code), and note that a student should trend toward the production norm as they grow.
- **FR-044**: `code-quality.md` MUST include a section on the mistakes AI coding agents commonly make - at minimum hallucinated or outdated APIs and packages, security vulnerabilities, overcomplication and unnecessary abstraction, ignoring existing patterns and the canonical host API, silent scope creep, and unverified or fabricated tests - with concrete guards; the skill MUST actively guard against these (verify every host-API call against the vendored `host_api.h`, build and run tests before claiming done, prefer existing patterns, keep changes minimal).

#### Spec-driven development active (Story 4)

- **FR-019**: The repository MUST ship with the spec-driven-development workflow pre-installed and active out of the box: its commands, templates, and project principles are present without any manual installation by the student.
- **FR-020**: The SDD workflow MUST be usable on first open; if a per-agent activation step is required, it MUST be performed automatically by the primary onboarding action (a "run first"/init step), not left to the student.
- **FR-021**: The SDD workflow MUST be available across the three target agents.
- **FR-022**: The SDD and AI features MUST be optional from the user's perspective: a student doing classical development can ignore them and still build, upload, and debug.

#### Knowledge base (Story 5)

- **FR-023**: The repository MUST make the badge knowledge locally available and cross-linked: the plugin SDK and examples, the host-API reference, the manifest/capability documentation, the serial-command reference, and the firmware feature specifications.
- **FR-024**: The knowledge MUST be vendored as an updatable reference (so that future firmware/SDK versions can be adopted) via a single documented update action, without hand-copying files.
- **FR-025**: The vendored knowledge MUST be obtained automatically as part of the primary onboarding action (the student never runs a separate, undocumented command to get it).
- **FR-026**: Cross-links between the course material, the examples, the host-API reference, and the firmware specifications MUST resolve within the repository.
- **FR-027**: The repository MUST treat the firmware's `host_api.h`/host-API reference as the single source of truth and MUST NOT introduce a competing, independently-edited copy of it.

#### README & official resources (Story 6)

- **FR-028**: The README's first section MUST state the repository's purpose, name both audiences (classical and vibe/SDD), and give the single recommended first action.
- **FR-029**: The README MUST provide two clearly labelled routes - a classical (agent-free) path and a vibe/SDD path naming the supported agents - both leading to working instructions.
- **FR-030**: The README MUST link to the official upstream resources: the firmware repository and documentation site, the plugin repository, the host-API reference, the web installer, and the spec-driven-development toolkit and supported-agent documentation.
- **FR-031**: The README MUST set the framing explicitly: this repository is an easy entry point to installing and running embedded software on the badge, for both classical and AI-assisted/SDD workflows.
- **FR-038**: The repository MUST include a beginner walkthrough (`EASY.md`) that takes an absolute beginner step by step, with copy-paste commands, from a bare computer to a running hello-world plugin on the badge. It MUST start from prerequisite installation - including how to install VS Code and git on each operating system: macOS via Homebrew (`brew`), Linux via `apt`, and Windows (download / `winget`) - then cover obtaining the repository, running the one-time setup, and the build/flash/run loop. It MUST give the exact prompts to type into an AI agent (Claude Code / Codex / opencode) for the vibe/SDD path and the equivalent `badge` CLI commands for the classical path. The README MUST link to it as the recommended starting point for absolute beginners.

#### Cross-cutting

- **FR-032**: The repository MUST NOT contain new firmware source; it packages, documents, and references the existing firmware and plugin projects. Firmware (C++/ESP-IDF) development is out of scope; the firmware's specifications and docs are included only as read-only knowledge.
- **FR-033**: All repository-authored documentation and instructions MUST describe the current state only (no history, rationale, migration notes, or references to past incidents), consistent with the upstream projects' documentation rules.
- **FR-034**: Course material that the agents read as instructions MUST be expressed in the cross-tool-portable convention so a single source serves the three agents rather than being duplicated and drifting per agent.
- **FR-037**: The repository MUST include two CI pipelines that build the development-container image and publish it to a registry: a GitHub Actions workflow that pushes to Docker Hub, and a GitLab CI pipeline that pushes to registry.krim.dev. Each pipeline MUST run only when its registry credentials are configured and MUST NOT affect local (non-CI) use.
- **FR-039**: The host-native path MUST be a first-class, correct experience on Windows (the majority of students), not only macOS/Linux: a single cross-platform bootstrap invoked the same way on every OS, COM-port auto-detection for flashing and serial, and OS-agnostic editor tasks and `badge` CLI behaviour. The bootstrap and tooling MUST be implemented in a cross-platform language (Python) and MUST avoid authored PowerShell (the maintainer does not use PowerShell); any unavoidable Windows wrapper MUST be reduced to a minimal one-line `.cmd` that only delegates to the Python script, never PowerShell scripting. Windows prerequisites are installed via documented `winget` commands run by the student, not by a maintainer-authored Windows script. It MUST rely on officially cross-platform tooling (rustup, Python, pyserial, Binaryen) and document the Windows specifics (the USB-CDC COM driver, COM naming).
- **FR-040**: Because the maintainer develops on macOS and cannot manually test Windows, the repository MUST include a cross-platform verification CI workflow - a build/bootstrap smoke matrix covering Windows, macOS, and Linux - that provisions the toolchain and builds the example plugin, so the Windows path is automatically verified on every change. Hardware-dependent steps (flash/serial) that CI cannot run MUST be exercised by their non-hardware portions where possible and otherwise clearly marked as manual.

### Key Entities *(include if feature involves data)*

- **Onboarding environment**: the reproducible, pinned definition of the development toolchain - a host-native bootstrap script (isolated via a Python venv plus a pinned rustup toolchain) as the primary path and a development container as the alternative - plus the editor configuration that makes build/flash/serial-monitor actions discoverable. Attributes: provisioning method(s), pinned versions, isolation mechanism, editor recommendations/tasks.
- **Automated flash & serial tooling**: the single-command flash action and the live serial-log reader, both invokable by the student and by an AI agent on the host. Attributes: invocation (non-interactive), port-contention handling, agent-usability, webflasher fallback.
- **Container image & publish pipelines**: the development-container image and the two CI pipelines that publish it. Attributes: image definition, GitHub Actions → Docker Hub, GitLab CI → registry.krim.dev, credential-gated execution.
- **Plugin starting point**: a copy-and-rename project skeleton with a manifest and minimal source that builds unmodified. Attributes: project layout, manifest fields (id, version, host-API level, memory, capabilities, i18n), lifecycle source skeleton.
- **AI assistant skill**: the complete plugin-development assistant available to the agents. Attributes: covered phases (develop/upload/debug), encoded pitfalls, knowledge sources it references, per-agent availability.
- **SDD workflow scaffolding**: the pre-installed, active spec-driven-development assets. Attributes: workflow commands, templates, project principles, activation mechanism.
- **Vendored knowledge sources**: the updatable references to the firmware and plugin projects (specifications, SDK, host-API reference, docs). Attributes: source projects, update mechanism, pinned version, cross-link targets.
- **README entry point**: the front-door document. Attributes: purpose statement, audience routing (classical vs vibe/SDD), first action, official-resource links.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A first-time student, starting from a clean machine with only the documented baseline, reaches a successfully built example plugin by performing the single primary onboarding action and one build action - with zero manual per-dependency installation.
- **SC-002**: From a ready environment, a student builds, flashes (one automated command), starts, and observes the live log output of a plugin on a physical badge by following the README, without consulting any source outside the repository.
- **SC-003**: The first successful build of an example plugin is reachable in no more than three discrete student actions after opening the repository (provision, build, observe), and the README's recommended first action is identifiable within the first screen.
- **SC-004**: In each of the three target agents (Claude Code, Codex, opencode), the assistant skill produces correct, badge-specific guidance for at least one development, one upload, and one debugging task, with no invented commands or constraints.
- **SC-005**: In each of the three target agents, the spec-driven-development workflow is usable on first open with no manual installation step performed by the student.
- **SC-006**: A maintainer advances the vendored knowledge to a newer upstream firmware/plugin release using a single documented action, with no hand-copying of files, and the course material remains valid without rewrites.
- **SC-007**: A classical developer completes the build/upload/debug loop while ignoring all AI and SDD features, confirming those features are optional.
- **SC-008**: Every cross-link from the course material to the host-API reference, SDK/manifest documentation, and firmware specifications resolves within the repository.
- **SC-009**: When the host cannot access the badge's USB port (e.g. the container alternative), uploading to a physically connected badge still succeeds via the documented WebSerial webflasher fallback.
- **SC-010**: An AI agent running on the host flashes an example plugin and reads its serial log using only the provided commands, with no manual steps.
- **SC-011**: Each container-publish pipeline (GitHub → Docker Hub, GitLab → registry.krim.dev), when its credentials are configured, builds and pushes the development-container image on its documented trigger.
- **SC-012**: An absolute beginner following `EASY.md` verbatim reaches a running hello-world plugin on the badge using only the listed copy-paste steps (agent prompts or `badge` commands), with no external lookups.
- **SC-013**: The cross-platform verification CI (Windows, macOS, Linux) provisions the toolchain and builds the example plugin green on all three operating systems, substituting for manual Windows testing.
- **SC-014**: A plugin feature generated via the SDD workflow includes automated tests by default, and `badge test` runs them green; the skill directs a test-first loop.
- **SC-015**: Code the skill generates follows the `code-quality.md` standards and is commented densely enough that a beginner can explain each step from the comments alone.

## Assumptions

- **Scope is plugin development, not firmware development**: the strong signal in the request ("alle Tools, um WASM-Plugins in Rust zu bauen", "TOTAL TRIVIAL") and the heavy weight of the firmware toolchain (PlatformIO + ESP-IDF) mean firmware C++ development is out of scope. The firmware's specifications and docs are bundled as read-only knowledge only.
- **Host-native primary, container alternative, local only (confirmed)**: the primary path is a host-native bootstrap (a single cross-platform Python script `scripts/setup.py`, with an optional thin bash wrapper for Unix and no authored PowerShell) targeting local VS Code, isolated via a Python venv plus a pinned rustup toolchain, so the AI agent and tooling have direct host USB access for flashing and serial logs. A development container is the reproducible/isolated build alternative. Browser/cloud editors (Codespaces) are out of scope.
- **Automated host-side flash by default, webflasher fallback (confirmed)**: the recommended default is a single automated flash command running directly on the host, with the agent reading serial logs directly; the browser-based WebSerial webflasher is the documented fallback for when the host has no USB access (e.g. the container alternative).
- **Container image published by two CI pipelines (confirmed)**: a GitHub Actions workflow builds and pushes the development-container image to Docker Hub, and a GitLab CI pipeline builds and pushes it to registry.krim.dev (Harbor), modelled on the existing selkies-gpu GitLab CI pattern (docker buildx build --push, optional cosign signing, default-branch trigger). Each runs only when its registry credentials are configured.
- **Knowledge vendored as git submodules (confirmed)**: git submodules of the firmware and plugin repositories provide updatable knowledge; the primary onboarding action initialises them automatically so students never manage them by hand, and one documented command advances them to a newer upstream release.
- **Speckit is the SDD toolkit and is committed/activated**: "Speckit vorinstalliert und aktiv" is assumed to mean the SDD scaffolding (principles, templates, workflow commands) is committed into the repository so it works immediately, with any per-agent activation handled automatically by the onboarding/init step.
- **Cross-tool portability via the shared agent-instructions convention**: a single shared instructions file plus each agent's native skill/command mechanism is assumed to be how one source serves Claude Code, Codex, and opencode without per-agent drift.
- **The skill's knowledge is harvested from existing material on this machine**: the assistant skill is built from the real firmware/SDK docs, the accumulated developer memory, and existing skills, rather than authored from scratch or as a placeholder.
- **Baseline a student already has**: an internet-connected computer. `EASY.md` covers installing the prerequisites themselves (VS Code and git, per OS) for absolute beginners; the User Stories above assume those prerequisites are in place. The optional container alternative additionally needs a container runtime. A physical badge is required only for the flash/run/debug stories, not for build-only learning.
- **Windows is the majority target and untestable by the maintainer (confirmed)**: most students use Windows while the maintainer develops on macOS, so Windows correctness is guaranteed by leaning on cross-platform tooling and a CI matrix (windows/macos/ubuntu) rather than manual testing; the browser webflasher is the Windows-safe upload route.
- **Audience and language**: the audience is students new to embedded and to AI-assisted/SDD workflows; repository-authored material follows the upstream "current state only" documentation rules and English-for-code conventions.
