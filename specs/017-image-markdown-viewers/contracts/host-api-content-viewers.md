# Contract: Content-Viewer Host API additions

Four new functions on the plugin host API. The canonical declaration lives in
`components/plugin_manager/include/plugin_manager/host_api.h` and MUST be mirrored
byte-identically into `~/GIT/cdc-badge-plugins/sdk/host_api.h` in the same change.

All calls push a host-managed viewer and return immediately; the user dismisses it
with back, which returns control to the plugin's previous view (same behavior as
`host_ui_push_info` / `host_fs_view`). Displaying content is **unprivileged**.

## Declarations

```c
/**
 * \brief Decode and display an image file from the plugin's sandbox.
 * \param name Bare filename in the plugin's vfat folder (no path separators).
 * \return HOST_OK, or HOST_ERR_* (NO_CAPABILITY if vfat missing, NOT_FOUND,
 *         INVALID_ARG for bad name / unsupported or oversized image).
 * Requires the `vfat` capability. Format auto-detected (PNG/JPEG/GIF).
 */
int host_fs_view_image(const char* name);

/**
 * \brief Render and display a Markdown file from the plugin's sandbox.
 * \param name Bare filename in the plugin's vfat folder.
 * \return HOST_OK or HOST_ERR_*. Requires the `vfat` capability.
 */
int host_fs_view_markdown(const char* name);

/**
 * \brief Decode and display an image from an in-memory buffer.
 * \param data Pointer to encoded image bytes in plugin linear memory.
 * \param len  Byte length (validated in full; must be <= 512 KB).
 * \return HOST_OK or HOST_ERR_INVALID_ARG (bad bounds / unsupported / oversized).
 * No capability required. Format auto-detected (PNG/JPEG/GIF).
 */
int host_ui_view_image(const uint8_t* data, uint32_t len);

/**
 * \brief Render and display Markdown from an in-memory buffer.
 * \param data Pointer to Markdown (UTF-8) bytes in plugin linear memory.
 * \param len  Byte length (validated in full; truncated at ~64 KB).
 * \return HOST_OK or HOST_ERR_INVALID_ARG (bad bounds).
 * No capability required.
 */
int host_ui_view_markdown(const uint8_t* data, uint32_t len);
```

## WAMR registration (`WamrImports.cpp`)

| Symbol | Wrapper validation | Signature |
|--------|--------------------|-----------|
| `host_fs_view_image` | name string (WAMR `$`) | `($)i` |
| `host_fs_view_markdown` | name string | `($)i` |
| `host_ui_view_image` | `wbuf_ok(env, data, len)` full extent | `(*~)i` |
| `host_ui_view_markdown` | `wbuf_ok(env, data, len)` full extent | `(*~)i` |

`*~` makes WAMR validate the whole `(ptr,len)`; the ui wrappers additionally assert
`wbuf_ok` before touching the buffer (defense in depth, Principle III).

## Behavior contract

- **Capability**: fs-family → requires `vfat` (via existing `resolvePath`, sandbox-confined). ui-family → no capability.
- **Size limits**: image source ≤ 512 KB and decoded ≤ ~1 MP, else `HOST_ERR_INVALID_ARG` and nothing is allocated. Markdown > ~64 KB is truncated with a visible indicator (still `HOST_OK`).
- **Format detection**: image calls sniff magic bytes; unknown → `HOST_ERR_INVALID_ARG`. No format parameter.
- **Failure**: malformed/empty input never crashes; the call frees all buffers and returns an error (or shows a readable error view). 
- **Navigation**: back from the viewer pops to the plugin's previous view.
- **Existing calls unchanged**: `host_fs_view` (plain text) keeps its current behavior; these are additive.

## Internal C++ surfaces (firmware-only, not plugin-facing)

```cpp
// components/cdc_image/include/cdc_image/ImageDecoder.h
namespace cdc::image {
  // Streaming decode of PNG/JPEG/GIF (auto-detected) into a dithered 1-bit bitmap.
  // Returns false on unsupported/oversized/corrupt input; allocates only in PSRAM.
  bool decodeToMonoBitmap(const uint8_t* data, size_t len, MonoBitmap& out, DecodeOpts opts);
}

// components/cdc_views/include/cdc_views/ImageView.h
namespace cdc::ui {
  ImageView* showImage(const char* title, MonoBitmap&& bmp);   // pushes onto ViewStack
}

// components/cdc_views/include/cdc_views/MarkdownView.h / MarkdownParser.h
namespace cdc::ui {
  MarkdownView* showMarkdown(const char* title, const char* src, size_t len); // parse + push
}
```

These are the seams the explorer (`VfatExplorerView::openEntry`) and the host calls
both use, so dispatch logic is written once.
