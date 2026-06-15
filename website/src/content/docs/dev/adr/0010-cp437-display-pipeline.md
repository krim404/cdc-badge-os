---
title: ADR-0010 — CP437 display pipeline
description: Draw text via render helpers; never gfx->print for user/i18n text.
sidebar:
  order: 10
---

**Status**: accepted
**Source**: spec 015/016 (display text); FR-094, FR-100; `components/cdc_views/src/RenderHelpers.cpp`

## Context

The text pipeline is canonical CP437: T9 input, plugin `to_display`, and the i18n loader all
produce CP437. The CalEPD submodule overrides `Epd::print(const std::string&)` with a hardcoded
UTF-8 assumption (`Epd::_unicodeEasy`): it skips bytes `0xC3`/`0xC2` and adds 64 to every byte in
`0x84..0xBE`. Applied to CP437 this corrupts text (`ae 0x84 -> 0xC4` renders as a line glyph,
`oe 0x94 -> 0xD4`), while `ue 0x81` and ASCII pass through. `gfx->print(char)` (single char)
routes to `Epd::print(const char)` and is not transformed.

## Decision

On-screen CP437 text is drawn through the render helpers, never through `gfx->print(const
char*)` / `gfx->print(std::string)` for user or i18n text.

- Built-in 6x8 font: `cdc::ui::render::printText(gfx, text)` (writes each byte via
  `gfx->write()`, bypassing the transform).
- Font-aware drawing: `cdc::ui::render::drawText(gfx, text, font)`.
- Shared chrome helpers (`printTruncated`, `drawHeaderLeft/Centered`, `drawFooterBar`) already do
  this internally.
- `gfx->print(...)` is acceptable only for pure-ASCII literals (e.g. `"[Y]"`, digit buffers).

i18n strings are CP437 at draw time: `lang_de.json` carries real UTF-8 umlauts and the loader
converts UTF-8 → CP437 (`cdc::core::cp437::fromUtf8`); in-code English fallbacks stay ASCII.

## Consequences

- Enables: correct rendering of extended glyphs (umlauts, box-drawing) across i18n, T9 input, and
  plugin output on the CalEPD display.
- Must hold: new views and helpers draw user/i18n text only via `printText`/`drawText`; in-code
  string literals drawn directly stay ASCII (no UTF-8 umlauts in code that bypasses the loader).
- Cost: developers cannot use the convenient `gfx->print` overload for non-ASCII text; the
  helper path is mandatory.
