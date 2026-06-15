# Feature Specification: Keypad / UI Interaction Model & ViewStack

**Feature Branch**: `016-keypad-ui`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the consistent
12-key interaction model (navigation, confirm/back, context menu, T9 text entry, sliders, digit
entry) and the rescue chord that forces the device back to a clean locked state.

> **Source of truth**: This spec lifts requirements FR-110..111 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are `cdc_views` and the `cdc_ui`
> ViewStack. USB keyboard auto-type (an output path, not the keypad input model) is owned by spec
> `005-password-vault` / the HID specs and is only referenced here as an open item.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Navigate every screen with one consistent key model (Priority: P1)

Every screen follows the same 12-key model: keys 2 and 8 move the selection (and hold to jump to the
first/last item), Y confirms or selects, N goes back / cancels / deletes, and 3 opens a context
menu. The holder never has to relearn navigation per screen.

**Why this priority**: A consistent input model is the foundation of the entire UI; inconsistent
keys would make every feature harder to use and test.

**Independent Test**: On a list view, press 2/8 to move selection, hold 2/8 to jump to first/last,
press Y to select, press N to go back, and press 3 to open the context menu — confirming each key
behaves as specified.

**Acceptance Scenarios**:

1. **Given** a list view, **When** the holder presses 2 or 8, **Then** the selection moves up/down by
   one; holding 2 or 8 jumps to the first/last item.
2. **Given** any selectable item, **When** the holder presses Y, **Then** the item is confirmed /
   selected; **When** the holder presses N, **Then** the device goes back / cancels / deletes.
3. **Given** a screen with a context menu, **When** the holder presses 3, **Then** the context menu
   opens.

---

### User Story 2 - Enter text, numbers and slider values (Priority: P2)

Text entry uses T9 multi-tap, sliders adjust with keys 4 and 6, and date/time and PIN fields use
direct digit entry. The same input primitives are reused everywhere a value must be entered.

**Why this priority**: Reusing T9, slider and digit-entry primitives keeps data entry consistent and
avoids per-screen one-off input handling.

**Independent Test**: In a text field enter a word via T9 multi-tap; on a slider press 4/6 and
confirm the value changes; in a PIN or date/time field enter digits directly.

**Acceptance Scenarios**:

1. **Given** a text-entry field, **When** the holder uses T9 multi-tap, **Then** characters are
   entered as in standard multi-tap input.
2. **Given** a slider, **When** the holder presses 4 or 6, **Then** the slider value decreases /
   increases.
3. **Given** a date/time or PIN field, **When** the holder presses digit keys, **Then** the digits
   are entered directly.

---

### User Story 3 - Stack modals so input never leaks to the screen behind (Priority: P2)

The ViewStack stacks modals rather than holding a single modal slot; while a modal is on top it
receives input exclusively, and input never leaks to the view behind it. Stacking and popping a
modal force a full composite of the screen.

**Why this priority**: Correct modal layering prevents the dangerous class of bugs where a
confirmation or warning is shown but the screen behind still reacts to keys.

**Independent Test**: Open a modal over a list view, press navigation keys, and confirm the list
behind does not move; dismiss the modal and confirm input returns to the list.

**Acceptance Scenarios**:

1. **Given** a modal on top of the ViewStack, **When** the holder presses any key, **Then** only the
   top modal receives the input and the view behind it does not react.
2. **Given** a modal is pushed or popped, **When** the stack changes, **Then** a full composite of
   the screen is forced.

---

### User Story 4 - Recover a wedged UI without a hardware reset (Priority: P2)

Holding the rescue chord (N + Y) forces the device back to a clean locked state: it unloads plugins
and dismisses dialogs without a hardware reset.

**Why this priority**: A software escape hatch lets the holder recover from a stuck UI (e.g. a
misbehaving plugin or a wedged dialog) without power-cycling and risking data loss.

**Independent Test**: With a plugin running and/or a dialog open, hold N + Y and confirm the device
returns to a clean lock screen with plugins unloaded and dialogs dismissed, without a reset.

**Acceptance Scenarios**:

1. **Given** any UI state (including a running plugin or an open dialog), **When** the holder holds
   the N + Y rescue chord, **Then** the device returns to a clean locked state — plugins unloaded,
   dialogs dismissed — without a hardware reset.

---

### Edge Cases

- **Hold-to-jump vs. single press**: a single 2/8 press moves by one item; a hold jumps to
  first/last.
- **Input leakage to background view**: while a modal is on top, no key reaches the view behind it;
  popping the modal restores input to the view below and forces a full composite.
- **Rescue chord during a plugin**: holding N + Y unloads the running plugin and dismisses dialogs,
  returning to a clean locked state without a reset.
- **Context menu availability**: key 3 opens a context menu only on screens that provide one.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-110**: Navigation MUST follow a consistent 12-key model: 2/8 move selection (hold to jump to
  first/last), Y confirm/select, N back/cancel/delete, 3 opens a context menu; text entry uses T9
  multi-tap; sliders use 4/6; date/time and PIN use digit entry.
  [NEEDS CLARIFICATION: USB keyboard auto-type — report format, inter-keystroke throttling, and the
  role of the serial `PASTE` command in that flow (output path referenced by spec 005). (baseline
  B11)]
- **FR-111**: A rescue chord (hold N+Y) MUST force the device back to a clean locked state (unload
  plugins, dismiss dialogs) without a hardware reset.
  [NEEDS CLARIFICATION: ViewStack modal-depth semantics are documented only at a high level (exact
  maximum modal stacking depth and overflow behaviour). (baseline B15)]

### Key Entities *(include if feature involves data)*

- **Keypad input model** — the consistent 12-key mapping: 2/8 selection move (hold = jump to
  first/last), Y confirm/select, N back/cancel/delete, 3 context menu; T9 multi-tap for text, 4/6
  for sliders, digit keys for date/time and PIN.
- **ViewStack** — the stack of views and modals; the top modal receives input exclusively and input
  never leaks to the view behind; push/pop of a modal forces a full composite. Exact modal-depth
  semantics per baseline B15.
- **Context menu** — the per-screen menu opened by key 3 on screens that provide one.
- **Rescue chord (N + Y)** — a held key combination that forces the device back to a clean locked
  state (plugins unloaded, dialogs dismissed) without a hardware reset.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Across every screen, the 12-key model behaves identically: 2/8 move selection (hold to
  jump first/last), Y confirms/selects, N goes back/cancels/deletes, and 3 opens a context menu
  where one exists.
- **SC-002**: T9 multi-tap text entry, 4/6 slider adjustment and direct digit entry each produce the
  expected value in their respective fields.
- **SC-003**: While a modal is on top of the ViewStack, 100% of key input goes to the top modal and
  none leaks to the view behind it; a modal push/pop forces a full composite.
- **SC-004**: Holding the N + Y rescue chord returns the device to a clean locked state (plugins
  unloaded, dialogs dismissed) without a hardware reset 100% of the time.

## Assumptions

- USB keyboard auto-type is an *output* path (HID keystrokes to a host), referenced by spec
  `005-password-vault` and the HID specs; this spec owns only the keypad *input* model and ViewStack
  behaviour, and the auto-type details (baseline B11) are left as a cross-referenced open item.
- Hardware is CDC Badge v1.0/v1.1 with a 12-button keypad; hardware-backed acceptance (key model,
  rescue chord, modal layering on the e-paper display) is verified on device and is non-blocking on
  the host test tier.
- The exact maximum ViewStack modal-depth and overflow behaviour (baseline B15) are read from source;
  the high-level "modals stack, top modal gets input exclusively" contract is the specified
  behaviour.
- On-screen text is drawn via the CP437-safe render path (owned by the display/i18n specs), not the
  raw print overload.
