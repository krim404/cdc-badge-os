---
title: Serial console
description: Connect to the badge over USB CDC, understand the AUTH login model, and use the always-available diagnostic commands.
sidebar:
  order: 5
---

The badge exposes a line-oriented command console over its USB CDC serial port.
It is the primary way to inspect device state, manage stored secrets, drive the
plugin and file-system shells, and reflash the firmware without touching the
hardware buttons.

This page covers connecting, the authentication model, and a practical
orientation. For the full per-command table see the
[serial command reference](/dev/proto/serial-commands/).

## Connecting

1. Plug the badge into a USB port. It enumerates as a USB CDC device.
2. Open the port at **115200 baud** with any serial terminal.
3. On connect the badge prints a banner:

```
=== CDC Badge OS Serial Console ===
Login with: AUTH <pin>
Type 'HELP' for available commands.
```

The `Login with: AUTH <pin>` line only appears in builds where secure serial is
enabled (off by default, see below). Press Enter or type `HELP` to confirm the
console is live.

On macOS the badge appears as `/dev/cu.usbmodem*`; on Linux as `/dev/ttyACM*`.
You can drive the console non-interactively, for example:

```bash
echo "PING" > /dev/ttyACM0   # expect: PONG
```

### Line editing

The console supports basic line editing:

| Key | Action |
|-----|--------|
| <kbd>Up</kbd> / <kbd>Down</kbd> | Walk command history (last 10 lines) |
| <kbd>Backspace</kbd> | Delete the previous character |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Cancel the current line |
| <kbd>Ctrl</kbd>+<kbd>U</kbd> | Clear the current line |

UTF-8 input (for example umlauts in a display name) is accepted and converted to
the badge's internal CP437 encoding as you type.

## The AUTH model

Whether a command needs authentication depends on a build flag and a per-command
flag.

### Secure serial (off by default)

`FEATURE_SECURE_SERIAL` defaults to **0 (off)**; it is enabled only when the
Kconfig option `CONFIG_SECURE_SERIAL` is set (see
[ADR-0012](/dev/adr/0012-build-profiles-and-defaults/)). With it on, every
command except `PING` and `AUTH` is rejected until you authenticate:

```
> STATUS
ERROR: Not authenticated. Use AUTH <pin> to login.
> AUTH 123456
OK: Authenticated
> STATUS
=== System Status ===
...
```

`AUTH <pin>` uses the badge PIN (the same PIN as the lock screen). After a
successful login the session stays authenticated until it is idle for
**5 minutes**, after which it times out and you must log in again. Each accepted
command resets the idle timer.

- `AUTH` with no argument logs you out (or prints usage if you were not logged
  in).
- `LOGOUT` ends the session explicitly.
- A wrong PIN drops any active session, so the next command runs unprivileged
  instead of inheriting the old one.

:::caution[PIN lockout applies here too]
The serial `AUTH` path shares the badge PIN retry counter and lockout with the
lock screen. While the badge is in a PIN lockout, only `PING` is accepted; every
other command (including `AUTH`) is refused until the lockout expires. Repeated
wrong PINs can lead to a permanent lockout depending on your security settings.
:::

In release builds, verbose `INFO`/`DEBUG` log output is also suppressed until a
session is authenticated; errors and warnings keep flowing so boot problems stay
visible.

### Per-command authentication

Some commands carry their own `requiresAuth` flag, so they need a logged-in
session **even in builds where secure serial is disabled**. These are the
commands that mutate persistent state or touch secret material, for example
`REBOOT`, `BOOTLOADER`, the whole `NVS`, `PIN`, `TR01`, `WIFI`, `MODULE`,
`BACKUP`, `GPG`, `TOTP`, `PASSWORD`, `VFAT`, `PLUGIN` and `LANG` groups.

The auth check is applied at the group level. That means even a read-only
subcommand such as `NVS LIST` or `TR01 STATUS` requires authentication, because
its parent command is gated. The only consistently open commands are the
diagnostics listed below plus the time/display setters.

Commands that do **not** require authentication: `HELP`, `PING`, `STATUS`,
`MEM`, `MEMINFO`, `CPU`, `ERROR_LOG`, `GET_TIME`, `GET_DATE`, `SET_TIME`,
`SET_DATE`, `SET_NAME`, `SET_INFO`, `SET_INFO2`, and the `VCARD` group. In a
build with secure serial enabled these still require a prior `AUTH`, except
`PING`.

## Discovering commands

`HELP` prints every registered command grouped by module, including the
subcommands of grouped commands such as `NVS`, `TR01` or `PLUGIN`:

```
> HELP
=== Available Commands ===

[system]
  HELP                 Show available commands
  PING                 Check if device is responsive
  ...
```

Grouped commands use `<GROUP> <SUBCOMMAND> [args]` syntax. Typing the bare group
name, `<GROUP> HELP`, or `<GROUP> ?` prints just that group's subcommand list:

```
> NVS
Usage: NVS <subcommand> [args]
Subcommands:
  LIST [namespace]           List entries (optional namespace filter)
  READ <ns> <key>            Read key value
  ...
```

## A practical orientation

A few things you will reach for often:

- **Liveness check:** `PING` returns `PONG`. It is the one command that always
  works, even during a PIN lockout.
- **System state:** `STATUS` shows free heap and uptime. `MEM` and `MEMINFO`
  give heap/PSRAM detail; `CPU` measures aggregate load over about 250 ms.
- **Reflash without buttons:** authenticate, then `BOOTLOADER` reboots into USB
  download mode so you can flash. After flashing, the chip reboots back into the
  new firmware automatically. See [First flash](/start/first-flash/) for the full
  procedure.
- **Restart:** `REBOOT` restarts the device (requires auth).
- **Clock:** `SET_TIME HH:MM:SS` and `SET_DATE DD.MM.YYYY` set the clock;
  `SET_DATE` also accepts a Unix timestamp, for example
  `echo "SET_DATE $(date +%s)" > /dev/ttyACM0`.
- **Inspect storage:** `NVS LIST` walks the non-volatile key/value store;
  `TR01 SLOTS` summarises secure-element slot usage.
- **Danger zone:** destructive commands require an explicit confirmation token,
  for example `NVS CLEAR YES` and `TR01 WIPE CONFIRM`. The `PIN DURESS` command
  arms a self-destruct PIN. Treat these with care.

## Binary uploads

A few commands (the `PLUGIN UPLOAD*` family and `VFAT RECEIVE`) switch the
console into a raw byte-streaming mode: the badge replies `READY`, then consumes
exactly the announced number of bytes from the serial stream and verifies a
CRC-32 before committing the file. While streaming, normal line parsing is
bypassed. Use the supplied tooling (for example `tools/upload.py`) rather than
pasting binary by hand.

## Next steps

- [Serial command reference](/dev/proto/serial-commands/) - the complete,
  source-verified command table.
- [First flash](/start/first-flash/) - reflash via the `BOOTLOADER` command.
