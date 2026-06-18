# Feature Specification: Image & Markdown Content Viewers

**Feature Branch**: `017-image-markdown-viewers`

**Created**: 2026-06-18

**Status**: Draft

**Input**: User description: "vFAT file viewer for image files (PNG, JPG, GIF) that displays them on the e-paper as well as possible via dithering, scaled (or scrollable), plus a basic Markdown parser that can be loaded into views, especially views that display scrollable text. Both must work in the vFAT explorer and also be triggerable by WASM plugins: a plugin can reference a file (in vFAT or in an in-memory buffer) that is then displayed. Text-file viewing already exists. The vFAT explorer must open Markdown files with the text viewer (rendered, not raw)."

## Clarifications

### Session 2026-06-18

- Q: What image sizes must the badge guarantee to open? → A: Up to ~1 megapixel (e.g. 1024×1024) and ≤ 512 KB source file size; larger sources are rejected with a readable message.
- Q: Does triggering image/Markdown display from a plugin require a manifest capability? → A: No. Displaying content is unprivileged. A sandboxed-file source is confined to the plugin's own sandbox and a buffer source touches only the plugin's own memory, so neither requires a capability; the host still validates the source (sandbox path / memory bounds / size limits).
- Q: How large a Markdown document must render fully before truncation? → A: Up to ~64 KB of Markdown source; longer documents are truncated with a visible indicator.
- Q: How are Markdown heading font sizes chosen? → A: Dynamically from the distinct heading levels present: start at the smallest heading font for the least-prominent level and step up one size per higher-prominence level (number of fonts used equals number of distinct levels). No fixed per-level mapping; `#` is not always the largest.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - View an image file from the file explorer (Priority: P1)

A user has copied a picture (`.png`, `.jpg`/`.jpeg`) onto the badge's file area. They open the file explorer, browse to the picture, and select it. The badge renders the picture on the black-and-white e-paper display, scaled to fit the screen, using dithering so photos and graphics remain recognizable despite having only two colors. They can press a key to switch to actual-size and pan/scroll around a larger image, and press back to return to the file list.

**Why this priority**: This is the headline new capability. The badge today cannot display any image; this story delivers a complete, demonstrable end-to-end slice on its own.

**Independent Test**: Place a known PNG and JPG on the file partition, open each from the explorer, confirm a recognizable rendering appears, toggle to actual-size and pan, and confirm back returns to the explorer. No plugin and no Markdown work is required for this story to deliver value.

**Acceptance Scenarios**:

1. **Given** a valid PNG on the file partition, **When** the user selects it in the explorer, **Then** the image is shown scaled-to-fit, centered, dithered to black/white, with the file name in the header.
2. **Given** a valid JPEG larger than the screen, **When** the user opens it, **Then** it is downscaled to fit while preserving aspect ratio.
3. **Given** an image displayed fit-to-screen, **When** the user presses the actual-size key, **Then** the image is shown at 1:1 and the directional keys pan/scroll across it.
4. **Given** a baseline or progressive JPEG, **When** the user opens it, **Then** it is decoded and rendered like any other image.
5. **Given** an image is on screen, **When** the user presses the back key, **Then** the explorer file list reappears at the previous position.
6. **Given** a corrupt or unsupported image file, **When** the user opens it, **Then** a readable error message is shown and the badge does not crash or hang.

---

### User Story 2 - Read a rendered Markdown file from the file explorer (Priority: P2)

A user stores a `.md` file (for example a note or a bundled README) on the badge. They open it from the file explorer and it appears in the scrollable text viewer **formatted**: headings stand out (rendered with larger bold fonts), list items are bulleted/numbered and indented, code is shown monospaced, block quotes and horizontal rules are visually distinct, and emphasis markup is applied rather than shown as raw `#`/`*`/`` ` `` characters. The user scrolls through the document with the existing up/down keys.

**Why this priority**: Markdown files already open in the badge today, but only as raw text. Rendering them is a refinement of an existing flow rather than a brand-new subsystem, so it ranks below the image viewer while still being independently valuable.

**Independent Test**: Place a `.md` file containing headings, lists, code and a block quote on the partition, open it from the explorer, and confirm the output is formatted (markup is interpreted, not printed literally) and scrolls. No image or plugin work is required.

**Acceptance Scenarios**:

1. **Given** a `.md` file with headings, lists, code spans and a block quote, **When** the user opens it from the explorer, **Then** each element is rendered with distinct formatting (headings in larger bold fonts) and the raw markup characters are not shown.
2. **Given** a Markdown document longer than one screen, **When** the user presses the scroll keys, **Then** the content scrolls and a position indicator reflects progress.
3. **Given** Markdown containing an element the renderer does not support (e.g. a table), **When** the file is opened, **Then** that element degrades to plain readable text without breaking the rest of the document.
4. **Given** a plain `.txt` file, **When** the user opens it, **Then** it continues to display as today (unformatted text), unaffected by the Markdown feature.

---

### User Story 3 - A plugin displays a referenced image or Markdown document (Priority: P3)

A WASM plugin wants to show rich content to the user, for example a help page or a generated chart. The plugin asks the host to display either an **image** or a **Markdown document**, providing the content as one of: a file in the plugin's own sandboxed file area, or an in-memory buffer the plugin has prepared. The host opens the appropriate viewer (image viewer or rendered Markdown text viewer); when the user dismisses it, control returns to the plugin's view.

**Why this priority**: This unlocks reuse of both viewers from third-party plugins, but it depends on the viewers from Stories 1 and 2 existing first.

**Independent Test**: With a test plugin, request display of (a) a bundled image file, (b) an image buffer, (c) a bundled Markdown file, and (d) a Markdown buffer; confirm each opens in the correct viewer and that back returns to the plugin.

**Acceptance Scenarios**:

1. **Given** a plugin with a bundled image in its sandbox, **When** it requests image display by file name, **Then** the image viewer opens with that image.
2. **Given** a plugin holding image bytes in its memory, **When** it requests image display by buffer, **Then** the image viewer opens with that image.
3. **Given** a plugin with Markdown text in a buffer, **When** it requests Markdown display, **Then** the rendered text viewer opens with the formatted content.
4. **Given** a plugin requesting a file outside its sandbox, or a buffer outside its valid memory, **When** the request is made, **Then** it is rejected with an error and the badge does not crash.
5. **Given** a viewer opened by a plugin, **When** the user presses back, **Then** the plugin's previous view is restored.

---

### Edge Cases

- **Oversized image** (dimensions or file size beyond the enforced limit): rejected with a readable message; no partial/garbled render, no crash.
- **Image with transparency**: transparent areas are composited onto a white background before dithering.
- **Corrupt / truncated JPEG**: reported as undecodable; no crash (decoder fails closed).
- **Truncated / malformed image stream**: detected during decode; viewer shows an error and frees all buffers.
- **Empty file** (image or Markdown): viewer shows an "empty file" message rather than a blank screen.
- **Very long Markdown** (beyond ~64 KB of source): content is truncated at the limit with a visible truncation indicator; scrolling still works for the shown portion.
- **Markdown with deeply nested or unsupported constructs**: degrades to plain text for those parts; the document still opens.
- **Markdown inline image reference** (`![alt](path)`): shown as its alt text (not decoded inline) in v1.
- **Low memory at open time**: viewer fails to open with a readable message and leaves the badge responsive; no leak of decode buffers.
- **Plugin buffer length mismatch / zero length**: rejected as an invalid argument.
- **Unsupported file extension in explorer**: behaves as today (image/Markdown handling only triggers for the recognized extensions).

## Requirements *(mandatory)*

### Functional Requirements

#### Image viewing

- **FR-001**: The file explorer MUST recognize `.png`, `.jpg`, and `.jpeg` files and open them in an image viewer rather than the text viewer.
- **FR-002**: The image viewer MUST decode the selected image and render it on the black-and-white display, scaled to fit the screen while preserving aspect ratio and centered.
- **FR-003**: The image viewer MUST apply dithering so that grayscale and color images are approximated as faithfully as the two-color display allows.
- **FR-004**: The image viewer MUST decode both baseline and progressive JPEGs.
- **FR-005**: Images with transparency MUST be composited onto a white background before rendering.
- **FR-006**: The image viewer MUST offer a fit-to-screen mode (default) and an actual-size mode in which the user can pan/scroll across images larger than the screen, switchable via a key.
- **FR-007**: The system MUST enforce a maximum source file size of 512 KB and maximum decoded dimensions of ~1 megapixel (e.g. 1024×1024); sources exceeding either limit MUST be rejected with a readable message.

#### Markdown rendering

- **FR-008**: The file explorer MUST open `.md` and `.markdown` files in the scrollable text viewer with Markdown rendering applied (formatted output, not raw markup).
- **FR-009**: The Markdown renderer MUST support at minimum: headings, ordered and unordered lists, inline code and fenced/indented code blocks, block quotes, horizontal rules, emphasis (bold/italic), links (rendered as their visible text), and wrapped paragraphs.
- **FR-010**: The Markdown renderer MUST degrade unsupported constructs to plain readable text without failing to open the document.
- **FR-011**: The Markdown rendering capability MUST be reusable by any scrollable text view, so that a view can load a Markdown source instead of plain text.
- **FR-012**: Headings and bold text MUST be rendered using the larger bold display fonts. The renderer MUST assign heading font sizes dynamically from the set of distinct heading levels actually present in the document: the least-prominent level present is rendered in the smallest heading font, and each higher-prominence level steps up one font size, so the number of distinct fonts used equals the number of distinct heading levels. It MUST NOT map each Markdown level number to a fixed font size (a document using only `#` does not force the largest font). The renderer supports mixed fonts and variable line heights within one document.
- **FR-013**: Plain text files (e.g. `.txt`) MUST continue to display unformatted, unaffected by Markdown rendering.

#### Plugin (WASM) integration

- **FR-014**: A plugin MUST be able to request display of an image, providing the source either as a file in its own sandboxed file area or as an in-memory buffer it owns.
- **FR-015**: A plugin MUST be able to request display of a Markdown document, providing the source either as a sandboxed file or as an in-memory buffer.
- **FR-016**: Displaying content MUST NOT require any manifest capability; it is an unprivileged operation. The host MUST nonetheless validate every plugin-supplied source: a sandboxed-file source MUST be confined to the plugin's own sandbox, and a buffer source MUST be validated against the plugin's memory bounds and the size limits. Invalid requests MUST be rejected without crashing.
- **FR-017**: When a viewer is opened on behalf of a plugin, dismissing it (back key) MUST return control to the plugin's previous view.
- **FR-018**: Existing plain-text file viewing for plugins MUST remain available; image and Markdown display are additive.

#### Cross-cutting

- **FR-019**: Malformed, unsupported, oversized, or empty inputs MUST never crash or hang the badge; every failure path MUST surface a readable message and release all buffers.
- **FR-020**: Decode and render buffers MUST be allocated from PSRAM, not internal RAM.
- **FR-021**: Both viewers MUST integrate with the existing view/navigation model so the back key returns to the originating context (explorer or plugin) and the header shows a meaningful title (file name or plugin-supplied title).
- **FR-022**: User-facing strings introduced by the viewers MUST be localizable via the existing i18n mechanism (English in-code fallback plus translation files).
- **FR-023**: The user and developer documentation website MUST be updated to describe the image viewer, Markdown rendering in the explorer, and the plugin display capability, as part of completing this feature.

### Key Entities *(include if feature involves data)*

- **Image file**: an encoded picture (`PNG` or `JPEG`, baseline or progressive) residing on the badge's file partition or provided as a plugin buffer. Attributes: format, encoded bytes, intrinsic dimensions.
- **Decoded image**: transient pixel data plus dimensions produced by decoding, held in PSRAM only while the viewer is open.
- **Markdown document**: a UTF-8 text source containing Markdown markup, originating from a file or a plugin buffer.
- **Rendered document**: the wrapped, styled lines (with per-line font/indent) derived from a Markdown source and consumed by the scrollable text viewer.
- **Display request**: an instruction (from the explorer or a plugin) to show content, carrying the content kind (image / Markdown / plain text) and the source (sandboxed file reference or in-memory buffer).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can open a valid PNG and JPEG (baseline or progressive) from the file explorer and see a recognizable rendering of each.
- **SC-002**: A typical photo (e.g. 640×480) scaled to fit the screen is visually recognizable after dithering.
- **SC-003**: Opening any supported image completes and shows the picture within 5 seconds of selection for sources within the size limit.
- **SC-004**: Opening a `.md` file shows formatted output: headings are visually distinct (larger bold), list items are bulleted/numbered, and no raw `#`/`*`/`` ` `` markup characters appear for supported elements.
- **SC-005**: 100% of malformed, oversized, or empty inputs (image or Markdown, from explorer or plugin) result in a readable error and a responsive badge, with zero crashes or hangs across a repeated test set.
- **SC-006**: A plugin can successfully trigger image display and Markdown display from both a sandboxed file and an in-memory buffer (four combinations), each opening the correct viewer.
- **SC-007**: After opening and closing any viewer repeatedly (at least 20 cycles), the badge remains responsive with no observable memory growth across cycles.
- **SC-008**: Plain-text file viewing in the explorer and via plugins behaves exactly as before the feature (no regression).

## Assumptions

- **Display is monochrome**: the e-paper is two-color (black/white); all color/grayscale content is reduced to black/white via dithering. This is a hardware constraint, not a configurable option.
- **JPEG via vendored libjpeg**: baseline and progressive JPEGs are decoded by the IJG libjpeg vendored under `components/libjpeg/` (the registry libjpeg-turbo does not build under PlatformIO); GIF is not supported. PNG uses the `espressif/libpng` registry component.
- **"Basic" Markdown scope**: the element set in FR-009 defines support. Tables, footnotes, raw HTML blocks, task lists, nested block quotes beyond one level, and inline-decoded images are out of scope for v1; inline image references render as alt text.
- **PSRAM is the right pool for buffers**: decode/render buffers go to PSRAM (internal RAM is the scarce resource). A plugin's WASM linear memory is PSRAM-backed, so passing an in-memory buffer is appropriate for plugin-generated or downloaded content, while a sandboxed file reference is appropriate for content bundled with the plugin. Both source kinds are supported.
- **Reuse of existing viewers**: the scrollable text viewer used today for text files is the host for rendered Markdown; the file explorer and plugin runtime are the two entry points, consistent with how text-file viewing already works.
- **Size limits are fixed**: images are capped at 512 KB source / ~1 megapixel decoded (FR-007), chosen so the decoded buffer fits comfortably in PSRAM without on-the-fly downscaling; Markdown documents render up to ~64 KB of source, with longer documents truncated and a visible indicator. All decode/render buffers live in PSRAM.
- **No on-device data migration**: per project policy (pre-1.0), no migration code is introduced; new viewers read current formats only.
- **No capability gating for display**: showing content in a host-managed viewer is unprivileged, like the existing on-screen info/text calls. A file source is confined to the plugin's own sandbox; a buffer source touches only the plugin's own memory. The only protections are sandbox-path confinement, memory-bounds checking, and the size limits.
