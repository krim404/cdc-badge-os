---
title: UI framework & display
description: IView, the ViewStack and its modal stack, input dispatch, the reusable views, the CP437 text pipeline, and the e-paper refresh modes.
sidebar:
  order: 4
---

The badge UI is a stack of views drawn to a 296x128 e-paper panel. This page
covers the `IView` contract, the `ViewStack` that navigates between views and
overlays modals, how key input is dispatched, the reusable views in
`cdc_views`, the critical CP437 text-drawing rule, and the e-paper refresh
modes.

## IView: the view contract

Every screen implements `IView`
(`components/cdc_ui/include/cdc_ui/IView.h:25`). The contract has four groups:

| Group | Methods | Source |
| --- | --- | --- |
| Lifecycle | `onEnter(void*)`, `onExit()`, `onResume()`, `onPause()` | `IView.h` |
| Rendering | `render(bool partial)`, `needsRender()`, `markDirty()`, `clearDirty()` | `IView.h:53`, `IView.h:58`, `IView.h:63`, `IView.h:70` |
| Input | `onKey(char)`, `onLongPress(char)` | `IView.h:88`, `IView.h:95` |
| Periodic / chrome / identity | `onTick(uint32_t)`, `getFooterHint()`, `setFooterHint(const char*)`, `getName()` | `IView.h:103`, `IView.h:111`, `IView.h:120`, `IView.h:127` |

`onEnter` is called when a view is pushed or becomes top; `onResume` when a
child is popped and it becomes visible again; `onPause` when it is covered by
another view or modal; `onExit` when it is removed. `render(bool partial)` takes
a partial-vs-full hint.

`ViewBase` also exposes `setLifecycleHooks(onHide, onShow, userData)`: opaque
callbacks fired from `onPause`/`onResume`, so a caller can react to its view
being hidden or shown without each view type knowing about it. `PluginUiState`
uses this to back the `host_ui_set_view_lifecycle` plugin host call.

`onKey` and `onLongPress` return an `InputResult` (`IView.h:10`):

| Value | Meaning |
| --- | --- |
| `CONSUMED` | Handled, may need re-render |
| `IGNORED` | Not handled |
| `REQUEST_POP` | View wants to be popped (back/cancel) |
| `REQUEST_PUSH` | View wants to push a child |

`ViewBase` (`IView.h:138`) is the default base: it manages the dirty flag,
stores a title and footer override, and provides default lifecycle handlers
that mark the view dirty on enter/resume (`IView.h:143-165`). A view that
animates or updates every tick must call `markDirty()` itself in `onTick()`
(`IView.h:64-70`).

## ViewStack: navigation

`ViewStack` is the singleton navigation stack
(`components/cdc_ui/include/cdc_ui/ViewStack.h:18`,
`components/cdc_ui/src/ViewStack.cpp:34`). Maximum depth is 20
(`ViewStack.h:20`).

| Operation | Behaviour | Source |
| --- | --- | --- |
| `push(view, ctx)` | Push and call `onEnter` | `ViewStack.cpp:174`, `ViewStack.cpp:72` |
| `pop()` | Pop top, `onExit` it, `onResume` the new top; never pops the root | `ViewStack.cpp:179`, `ViewStack.cpp:94` |
| `replace(view, ctx)` | Swap the top view in place | `ViewStack.cpp:184` |
| `popToRoot()` | Pop down to the root view | `ViewStack.cpp:214` |
| `popToAnchor(anchor)` | Pop until `anchor` is current | `ViewStack.cpp:221` |
| `popToDepth(n)` | Pop until depth is at most `n` | `ViewStack.cpp:230` |
| `current()` / `at(i)` / `depth()` | Inspect the stack | `ViewStack.cpp:237`, `ViewStack.cpp:243`, `ViewStack.h:83` |

`pop()` refuses to remove the last view (`ViewStack.cpp:95-98`). On push/pop the
stack decides whether the next paint is a full refresh: a list-to-list
transition stays partial, everything else forces a full refresh
(`ViewStack.cpp:89`, `ViewStack.cpp:117`).

The stack is guarded by a FreeRTOS **recursive** mutex because it is touched
from multiple tasks (UI task, USB CTAP-HID task, BLE callback task) and because
view callbacks fired from inside `dispatchKey`/`dispatchTick` legitimately call
back into `push`/`pop`/`showModal` on the same task (`ViewStack.cpp:1-12`,
`ViewStack.cpp:43-47`).

### Exclusive lock

A caller (for example a FIDO2 prompt) can take exclusive ownership of the stack
with `acquireExclusive(owner)` (`ViewStack.h:177`, `ViewStack.cpp:471`). While
held, `push`/`pop`/`replace`/`showModal` from anyone other than the owner are
rejected with a warning (`ViewStack.cpp:77-81`, `ViewStack.cpp:101-105`,
`ViewStack.cpp:386-390`). Release with `releaseExclusive(owner)`
(`ViewStack.cpp:486`).

## The modal stack

Modals (toasts, context menus, confirm dialogs) are a **stack**, not a single
slot. `showModal(modal)` pushes onto the modal stack on top of any modal
already shown; `hideModal()` removes the top one, revealing the modal beneath it
(or the base view) (`ViewStack.h:127-138`, `ViewStack.cpp:383`,
`ViewStack.cpp:419`). Maximum modal depth is 4 (`ViewStack.h:219`); when full,
the oldest modal at the bottom is dropped to make room (`ViewStack.cpp:403-407`).

Key behaviours:

- Re-showing an already-stacked modal lifts it back to the top instead of
  duplicating it, so the shared toast/confirm singletons re-show in place
  (`ViewStack.cpp:393-400`).
- `removeModal(modal)` removes a specific modal from any position, used when the
  backing view object is about to be destroyed (e.g. plugin teardown) so the
  stack keeps no dangling pointer (`ViewStack.h:141-149`, `ViewStack.cpp:142`).
- `hasModal()` / `getModal()` report and return the top input-receiving modal
  (`ViewStack.h:154`, `ViewStack.h:159`).

Stacking or dismissing a modal forces a full composite so a smaller new modal
does not leave the previous one's edges showing, and a dismissed modal is fully
erased (`ViewStack.cpp:409-412`, `ViewStack.cpp:128-130`).

## Input dispatch

The UI tick reads the keypad and feeds characters into the stack. The keypad
`Key` enum maps directly to characters: `'0'`-`'9'`, `'Y'` (yes/select), `'N'`
(no/back) (`components/cdc_hal/include/cdc_hal/IKeypad.h:16-21`). `ui_process`
reads the next key and calls `dispatchKey`
(`components/cdc_os_ui/src/AppUi.cpp:1109-1113`).

Dispatch rules (`ViewStack.cpp:249`):

- If a modal is shown, the key goes to the **top modal only** and never falls
  through to the view (or lower modals) behind it
  (`ViewStack.cpp:255-263`).
- Otherwise the key goes to the current view; a returned `REQUEST_POP` triggers
  a `pop()` (`ViewStack.cpp:265-271`).

Long-press dispatch (`ViewStack.cpp:274`) follows the same modal-first rule and
additionally treats `'N'` as the universal back/cancel gesture: it pops (or
hides the top modal) unless the view/modal consumed the press itself
(`ViewStack.cpp:283-301`).

`dispatchTick` ticks the top modal and the current view each iteration
(`ViewStack.cpp:304`); the main UI tick calls it once per loop
(`AppUi.cpp:1122`).

## Reusable views (cdc_views)

`components/cdc_views/` holds views reusable by any module or by core. Each is a
`ViewBase` subclass with documented keys. Examples:

| View | Purpose | Source |
| --- | --- | --- |
| `ListView` | Scrollable selection menu; keys 2=up, 8=down, Y=select, N=back, 3=context | `components/cdc_views/include/cdc_views/ListView.h:34`, `ListView.h:28-32` |
| `T9InputView` | Phone-style multi-tap text entry | `components/cdc_views/include/cdc_views/T9InputView.h:20`, `T9InputView.h:14-18` |
| `PinEntryView` | PIN entry | `components/cdc_views/include/cdc_views/PinEntryView.h` |
| `SliderView` | Numeric slider | `components/cdc_views/include/cdc_views/SliderView.h` |
| `ConfirmView` / `MessageBox` / `ToastView` | Confirm dialog, message box, toast | `components/cdc_views/include/cdc_views/ConfirmView.h`, `MessageBox.h`, `ToastView.h` |
| `InfoView` / `QRCodeView` / `CanvasView` | Scrollable info text, QR code, freeform canvas | `components/cdc_views/include/cdc_views/InfoView.h`, `QRCodeView.h`, `CanvasView.h` |
| `ContextMenuView` / `ColorPickerView` / `DateInputView` / `TimeInputView` | Context menu and pickers | `components/cdc_views/include/cdc_views/ContextMenuView.h`, `ColorPickerView.h`, `DateInputView.h`, `TimeInputView.h` |

`ListView` stores its item array by pointer (not copied) and supports an
optional recursive mutex so a cross-task writer can re-point the backing array
safely (`ListView.h:67-90`). The on-screen row count is fixed at 4 for the
296x128 panel (`ListView.h:38-39`).

Module-specific views (a view only one module uses) belong inside that module,
not in `cdc_views`. See [Developing a module](/dev/module-development/).

## CP437 text: never gfx->print

:::danger[Do not call gfx->print for user/i18n text]
The whole text pipeline (T9 input, i18n, plugin display strings) is canonical
**CP437**. The CalEPD `Epd::print(const std::string&)` overload assumes UTF-8:
it adds 64 to bytes `0x84..0xBE`, which corrupts CP437 text (for example `ae`
`0x84` becomes `0xC4`, a box-line glyph). Use the render helpers instead, which
write each byte straight through `gfx->write()`, bypassing that transform
(`components/cdc_views/src/RenderHelpers.cpp:16-27`).
:::

The correct entry points are in `cdc::ui::render`
(`components/cdc_views/include/cdc_views/RenderHelpers.h:12`):

| Helper | Use | Source |
| --- | --- | --- |
| `printText(gfx, text)` | Built-in 6x8 glcdfont; writes raw CP437 bytes | `RenderHelpers.h:114`, `RenderHelpers.cpp:442` |
| `drawText(gfx, text, font)` | Font-aware: raw bytes for the built-in font, CP437-to-Latin1 mapping for TTF-derived GFX fonts | `RenderHelpers.h:102`, `RenderHelpers.cpp:428` |
| `drawCp437Text(gfx, text)` | Maps each CP437 byte to Latin-1 for Latin-1-indexed GFX fonts | `RenderHelpers.h:89`, `RenderHelpers.cpp:421` |
| `printTruncated`, `drawHeaderLeft/Centered`, `drawFooterBar` | Shared chrome; already byte-safe internally | `RenderHelpers.h:19-40`, `RenderHelpers.cpp:40` |

Internally all three text helpers funnel through `writeRaw`, which loops over
the bytes and calls `gfx->write(byte)` so nothing is transformed
(`RenderHelpers.cpp:23-27`, `RenderHelpers.cpp:436`, `RenderHelpers.cpp:445`).
`drawText`/`drawCp437Text` additionally map CP437 to Latin-1 per byte for
TTF-derived fonts whose glyph index space is Latin-1
(`RenderHelpers.cpp:434-439`).

`gfx->print(...)` is acceptable only for pure-ASCII literals (e.g. `"[Y]"` or a
digit buffer), since ASCII is below the corrupting range.

## E-paper refresh modes

The display HAL defines three refresh modes
(`components/cdc_hal/include/cdc_hal/IDisplay.h:11`):

| Mode | Meaning | Source |
| --- | --- | --- |
| `FULL` | Full refresh, slow, no ghosting | `IDisplay.h:12` |
| `PARTIAL` | Fast partial refresh; may ghost; periodically promoted to FULL to clear ghosting | `IDisplay.h:13` |
| `PARTIAL_LIGHT` | Partial refresh that is never promoted to FULL; for tiny low-churn updates | `IDisplay.h:14` |

`flush(mode)` (async) and `flushSync(mode)` (blocking) both default to `PARTIAL`
(`IDisplay.h:33`, `IDisplay.h:39`).

The `ViewStack::render()` path chooses the mode (`ViewStack.cpp:315`):

- After a view change (push/pop/replace/modal change) the stack sets
  `needsFullRefresh_`, so the next paint is `FULL`
  (`ViewStack.cpp:364-366`, `ViewStack.cpp:89`, `ViewStack.cpp:117`).
- Otherwise, for a plain repaint the mode is `PARTIAL_LIGHT` when the current
  view returns `prefersLightRefresh() == true`, else `PARTIAL`
  (`ViewStack.cpp:364-366`).
- When a modal is shown, a dirty base under a still-visible modal repaints with
  `PARTIAL`; `FULL` is reserved for modal-stack changes so a dismissed modal is
  erased (`ViewStack.cpp:347-353`).

`prefersLightRefresh()` defaults to false (`IView.h:79`). The lock-screen view
overrides it to true so its once-a-minute clock update is never promoted to a
flickering full refresh while the badge sits idle
(`components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:120`,
`IView.h:73-79`). A full refresh still happens on the next view change.

`render(synchronous)` flushes via `flushSync` when synchronous and `flush`
otherwise; the main UI tick calls the async path
(`ViewStack.cpp:367-370`, `AppUi.cpp:1131-1132`), while the sleep path flushes
synchronously so the panel is settled before sleeping
(`components/cdc_os_ui/src/SleepManager.cpp:120`).
