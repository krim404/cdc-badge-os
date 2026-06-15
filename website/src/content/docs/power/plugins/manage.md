---
title: Managing plugins
description: Start, stop, disable, and delete plugins on the CDC Badge, and understand background, autoload, and prevent_sleep residency.
sidebar:
  order: 3
---

Once a plugin is installed you manage it either from the on-device **Plugins** menu or over the serial console. Both act on the same plugin state.

## On-device Plugins menu

Open **Plugins** from the main menu. The list shows every installed plugin by its display name. Two list icons signal state:

- A sun icon marks a plugin that is currently running in the background.
- An `X` marks a plugin that has been disabled.

Actions:

- **Start:** select a plugin (the normal select key) to start it and open its view.
- **Context menu:** press <kbd>3</kbd> on a plugin to open its action menu. The entries depend on state:
  - Disabled plugin: **Enable**.
  - Running in the background: **Stop**, **Disable**.
  - Stopped (and not disabled): **Start**, **Disable**.

**Stop** force-unloads the plugin from RAM (its files stay on disk). **Disable** unloads it and writes a persistent marker so it will not start, including at boot. **Enable** removes that marker.

## Residency: foreground, background, autoload

At most one plugin is the foreground plugin (the one whose view you are looking at) at any time. What happens when you leave that view, and whether a plugin runs at boot, is controlled by three manifest capabilities:

| Capability | Effect |
| --- | --- |
| (none) | When you leave the plugin's view it is unloaded. |
| `background` | The plugin keeps running and ticking after you leave its view, instead of being unloaded. It is demoted to a resident background slot. This is **not** a boot flag: you still start it manually. |
| `autoload` | The plugin is started as a resident background instance at badge boot (headless: no foreground view). Disabled plugins are skipped. |
| `prevent_sleep` | While the plugin is loaded it holds a sleep inhibitor, and the idle auto-lock does not fire while it holds the foreground. |

`background` and `autoload` are orthogonal: `autoload` governs starting at boot, `background` governs survival after you leave the view. A plugin without `autoload` stays unloaded until you start it.

When you leave the view of a `background` plugin, the badge shows a brief "runs in background" toast and then demotes the plugin off the foreground.

## Status indicators on the lock screen

- A **background** status icon appears whenever any plugin is resident in the background slot.
- The **caffeinated** (sleep-inhibited) icon appears when a sleep inhibitor is held, for example by a `prevent_sleep` plugin.

So a `background` plugin that also sets `prevent_sleep` makes both icons visible.

## Serial commands

The `PLUGIN` command group mirrors the on-device actions:

```text
PLUGIN LIST              # installed plugins as JSON (id, name, version, disabled)
PLUGIN INFO <id>         # manifest details: version, api_level, caps, resources, prereqs
PLUGIN START <id>        # start a plugin
PLUGIN STOP              # stop the currently active plugin
PLUGIN CMD <id> <args>   # forward a command string to a plugin
PLUGIN ENABLE <id>       # remove the disabled marker
PLUGIN DISABLE <id>      # disable and unload from RAM
PLUGIN DELETE <id>       # delete the plugin's files
PLUGIN DEBUG             # toggle verbose plugin/host_* logging
```

`PLUGIN DELETE` removes every file for that id: `.wasm`, `.aot`, `.meta`, `.lang`, and `.disabled`. `PLUGIN DISABLE` unloads the plugin from RAM and writes the persistent disabled marker, so it will not start until re-enabled.

The same operations are available through `tools/upload.py` (`--list`, `--info`, `--start`, `--stop`, `--delete`). See [Installing plugins](/power/plugins/install/).
