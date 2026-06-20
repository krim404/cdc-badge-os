# Contract: `badge` CLI

The single automated interface for the build/flash/debug loop. Runs from the project Python venv (`tools/badge.py`, optionally exposed as a `badge` console script). Every subcommand is non-interactive and scriptable so an AI agent can drive it directly (FR-009, FR-035). Each VS Code task in `.vscode/tasks.json` maps to one subcommand.

## Subcommands

| Command | Arguments | Behavior | Exit |
|---------|-----------|----------|------|
| `badge new <name>` | `<name>` (valid plugin id) | Copy `vendor/cdc-badge-plugins/sdk/plugin_template_rust` into `plugins/<name>/`, rename crate + `meta.json id` | 0 on success; non-zero if name invalid or exists |
| `badge build <name>` | `<name>` | `cargo build --release --target wasm32-unknown-unknown -p <name>` then `wasm-opt -Oz --enable-bulk-memory --enable-nontrapping-float-to-int`; print artifact path | 0 + path; non-zero on compile/opt error |
| `badge test <name>` | `<name>` | `cargo test -p <name>` on the host (native) target — the TDD loop for plugin logic | 0 if tests pass; non-zero on failure |
| `badge flash <name>` | `<name>`, `[--start]`, `[--pin <pin>]`, `[--port <port>]`, `[--monitor]` | Build if stale, then call `vendor/cdc-badge-os/tools/upload.py --wasm … --meta … [--lang …] [--pin …]`; with `--start` start the plugin; with `--monitor` stream logs after | 0 on badge `OK`; non-zero on `ERR`/timeout with reason |
| `badge monitor` | `[--port <port>]`, `[--seconds <n>]`, `[--until <marker>]` | Open port @115200, stream decoded lines to stdout; bounded mode exits after N seconds or on marker (agent-friendly) | 0 on clean exit; non-zero if port busy/inaccessible |
| `badge list` | — | `PLUGIN LIST` → table/JSON of installed plugins | 0 |
| `badge start <id>` / `badge stop` | `<id>` | `PLUGIN START <id>` / `PLUGIN STOP` | 0/err |
| `badge delete <id>` | `<id>` | `PLUGIN DELETE <id>` | 0/err |

## Invariants

- **Port ownership**: `flash` and `monitor` hold the serial port exclusively; on a busy port the command prints a clear, actionable message (e.g. "serial port in use — close the monitor first") rather than hanging (FR-036).
- **Auth**: when the badge requires a PIN, `--pin` (or a prompted/env value) is forwarded to `AUTH <pin>` before PLUGIN commands (FR-013).
- **Port detection**: auto-detect `/dev/cu.usbmodem*` / `/dev/ttyACM*` / `/dev/ttyUSB*` / `COM*`; override with `--port`.
- **Cross-platform (Windows first-class, CI-verified)**: pure-Python, no shell-isms; `COM*` handled via pyserial on Windows; VS Code tasks invoke the venv Python OS-agnostically. The verification CI matrix (windows/macos/ubuntu) builds the example on all three (FR-039, FR-040). On Windows the bulletproof upload route is the WebSerial webflasher.
- **No reimplementation**: the upload protocol (PLUGIN UPLOAD / UPLOAD_META / UPLOAD_LANG, CRC32, READY/OK) is delegated to upstream `upload.py`, not reimplemented.
- **Fallback**: when the host cannot access USB (container alternative), the documented fallback is the WebSerial webflasher (`vendor/cdc-badge-plugins/webflasher`, also hosted upstream) — not this CLI (FR-012).

## VS Code tasks (`.vscode/tasks.json`)

| Task label | Runs |
|------------|------|
| `Setup` | `python scripts/setup.py` (one-time bootstrap; identical on every OS, no PowerShell) |
| `Build plugin` | `badge build <input>` |
| `Test plugin` | `badge test <input>` |
| `Flash plugin` | `badge flash <input> --start --monitor` |
| `Monitor serial` | `badge monitor` |
