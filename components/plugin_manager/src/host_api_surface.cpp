/**
 * \file host_api_surface.cpp
 * \brief Offscreen surface host API: canvas-style drawing onto MonoSurface
 *        targets of arbitrary size, exported as packed 1-bpp or JPEG.
 *
 * Surfaces are per-plugin (max HOST_SURFACE_MAX_PER_PLUGIN, each capped at
 * HOST_SURFACE_MAX_BYTES of pixel data) and are freed on plugin unload. All
 * calls run inside the owner's WASM frame, so the table needs no locking.
 * On the surface, ink is color 1 (a set bit is black) and background is 0.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/PluginUiState.h"
#include "cdc_image/JpegEncoder.h"
#include "cdc_views/CanvasView.h"
#include "cdc_views/Fonts.h"
#include "cdc_views/MonoSurface.h"
#include "cdc_views/RenderHelpers.h"
#include "host_str_conv.h"

#include <cstring>
#include <memory>
#include <string>

namespace pm = cdc::plugin_manager;
namespace render = cdc::ui::render;

extern "C" void* plg_get_active_plugin(void);

namespace {

constexpr uint16_t SURFACE_MAX_W = 1024;
constexpr uint16_t SURFACE_MAX_H = 2048;
constexpr uint16_t INK = 1;

struct SurfaceSlot {
    std::unique_ptr<cdc::ui::MonoSurface> surf;
    void*    plugin = nullptr;
    uint32_t handle = 0;
    uint8_t  font_id = 0;
    uint8_t  text_size = 1;
    uint8_t  shade = 255;
    bool     inverted = false;
};

// Global pool: at most HOST_SURFACE_MAX_PER_PLUGIN per plugin, and only a
// handful of plugins are resident at once.
constexpr uint8_t MAX_SLOTS = 8;
SurfaceSlot s_slots[MAX_SLOTS];
uint32_t s_next_handle = 1;

SurfaceSlot* find_slot(uint32_t handle) {
    if (handle == 0) return nullptr;
    void* plugin = plg_get_active_plugin();
    for (auto& s : s_slots) {
        if (s.surf && s.handle == handle && s.plugin == plugin) return &s;
    }
    return nullptr;
}

// Applies the slot's font/size/color state before text drawing or measuring.
const GFXfont* apply_text_state(SurfaceSlot& s) {
    const GFXfont* font = cdc::ui::getGfxFont(s.font_id);
    s.surf->setFont(font);
    s.surf->setTextSize(s.text_size);
    s.surf->setTextColor(s.inverted ? 0 : INK);
    s.surf->setTextWrap(false);
    return font;
}

}  // namespace

extern "C" {

int host_surface_create(uint16_t w, uint16_t h) {
    if (w == 0 || h == 0 || w > SURFACE_MAX_W || h > SURFACE_MAX_H) {
        return HOST_ERR_INVALID_ARG;
    }
    const size_t bytes = static_cast<size_t>((w + 7) / 8) * h;
    if (bytes > HOST_SURFACE_MAX_BYTES) return HOST_ERR_INVALID_ARG;

    void* plugin = plg_get_active_plugin();
    if (!plugin) return HOST_ERR_GENERIC;

    uint8_t owned = 0;
    for (const auto& s : s_slots) {
        if (s.surf && s.plugin == plugin) owned++;
    }
    if (owned >= HOST_SURFACE_MAX_PER_PLUGIN) return HOST_ERR_NO_MEMORY;

    for (auto& s : s_slots) {
        if (s.surf) continue;
        auto surf = std::make_unique<cdc::ui::MonoSurface>(w, h);
        if (!surf->ok()) return HOST_ERR_NO_MEMORY;
        s.surf = std::move(surf);
        s.plugin = plugin;
        s.handle = s_next_handle++;
        s.font_id = 0;
        s.text_size = 1;
        s.shade = 255;
        s.inverted = false;
        return static_cast<int>(s.handle);
    }
    return HOST_ERR_NO_MEMORY;
}

int host_surface_destroy(uint32_t surface) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    *s = SurfaceSlot{};
    return HOST_OK;
}

int host_surface_clear(uint32_t surface) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->surf->clear();
    return HOST_OK;
}

int host_surface_set_font(uint32_t surface, uint8_t font_id) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (font_id >= cdc::ui::kFontIdCount) return HOST_ERR_INVALID_ARG;
    s->font_id = font_id;
    return HOST_OK;
}

int host_surface_set_text_size(uint32_t surface, uint8_t size) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (size < 1 || size > 4) return HOST_ERR_INVALID_ARG;
    s->text_size = size;
    return HOST_OK;
}

int host_surface_set_text_color(uint32_t surface, uint8_t inverted) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->inverted = inverted != 0;
    return HOST_OK;
}

int host_surface_set_shade(uint32_t surface, uint8_t shade) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->shade = shade;
    return HOST_OK;
}

int host_surface_draw_text(uint32_t surface, int16_t x, int16_t y, const char* text) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!text) return HOST_ERR_INVALID_ARG;
    const GFXfont* font = apply_text_state(*s);
    std::string cp = pm::toDisplay(text);
    s->surf->setCursor(x, y);
    render::drawText(s->surf.get(), cp.c_str(), font);
    return HOST_OK;
}

int host_surface_draw_text_aligned(uint32_t surface, int16_t x, int16_t y, int16_t w,
                                   const char* text, uint8_t align) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!text || align > 2) return HOST_ERR_INVALID_ARG;
    const GFXfont* font = apply_text_state(*s);
    std::string cp = pm::toDisplay(text);

    int16_t draw_x = x;
    if (align == 1 || align == 2) {
        int16_t bx, by;
        uint16_t bw, bh;
        render::measureText(s->surf.get(), cp.c_str(), font, 0, 0, &bx, &by, &bw, &bh);
        if (align == 1) draw_x = x + (w - static_cast<int16_t>(bw)) / 2;
        else            draw_x = x + w - static_cast<int16_t>(bw);
    }
    s->surf->setCursor(draw_x, y);
    render::drawText(s->surf.get(), cp.c_str(), font);
    return HOST_OK;
}

int host_surface_measure_text(uint32_t surface, const char* text,
                              uint16_t* out_w, uint16_t* out_h) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!text || !out_w || !out_h) return HOST_ERR_INVALID_ARG;
    const GFXfont* font = apply_text_state(*s);
    std::string cp = pm::toDisplay(text);
    int16_t bx, by;
    render::measureText(s->surf.get(), cp.c_str(), font, 0, 0, &bx, &by, out_w, out_h);
    return HOST_OK;
}

int host_surface_draw_pixel(uint32_t surface, int16_t x, int16_t y) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->surf->drawPixel(x, y, INK);
    return HOST_OK;
}

int host_surface_draw_line(uint32_t surface, int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->surf->drawLine(x0, y0, x1, y1, INK);
    return HOST_OK;
}

int host_surface_draw_rect(uint32_t surface, int16_t x, int16_t y, int16_t w, int16_t h,
                           uint8_t filled) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!filled) {
        s->surf->drawRect(x, y, w, h, INK);
    } else if (s->shade >= 255) {
        s->surf->fillRect(x, y, w, h, INK);
    } else if (s->shade > 0) {
        render::fillRectDither(s->surf.get(), x, y, w, h, s->shade, INK);
    }
    return HOST_OK;
}

int host_surface_draw_circle(uint32_t surface, int16_t x, int16_t y, int16_t r,
                             uint8_t filled) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!filled) {
        s->surf->drawCircle(x, y, r, INK);
    } else if (s->shade >= 255) {
        s->surf->fillCircle(x, y, r, INK);
    } else if (s->shade > 0) {
        render::fillCircleDither(s->surf.get(), x, y, r, s->shade, INK);
    }
    return HOST_OK;
}

int host_surface_draw_triangle(uint32_t surface, int16_t x0, int16_t y0,
                               int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                               uint8_t filled) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!filled) {
        s->surf->drawTriangle(x0, y0, x1, y1, x2, y2, INK);
    } else if (s->shade >= 255) {
        s->surf->fillTriangle(x0, y0, x1, y1, x2, y2, INK);
    } else if (s->shade > 0) {
        render::fillTriangleDither(s->surf.get(), x0, y0, x1, y1, x2, y2, s->shade, INK);
    }
    return HOST_OK;
}

int host_surface_draw_round_rect(uint32_t surface, int16_t x, int16_t y,
                                 int16_t w, int16_t h, int16_t r, uint8_t filled) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (filled) s->surf->fillRoundRect(x, y, w, h, r, INK);
    else        s->surf->drawRoundRect(x, y, w, h, r, INK);
    return HOST_OK;
}

int host_surface_hline(uint32_t surface, int16_t x, int16_t y, int16_t w) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->surf->drawFastHLine(x, y, w, INK);
    return HOST_OK;
}

int host_surface_vline(uint32_t surface, int16_t x, int16_t y, int16_t h) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    s->surf->drawFastVLine(x, y, h, INK);
    return HOST_OK;
}

int host_surface_draw_bitmap(uint32_t surface, int16_t x, int16_t y,
                             int16_t w, int16_t h, const uint8_t* data, uint32_t len) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!data || w <= 0 || h <= 0) return HOST_ERR_INVALID_ARG;
    const uint32_t needed = static_cast<uint32_t>((w + 7) / 8) * static_cast<uint32_t>(h);
    if (len < needed) return HOST_ERR_INVALID_ARG;
    s->surf->drawBitmap(x, y, data, w, h, INK);
    return HOST_OK;
}

int host_surface_export(uint32_t surface, uint8_t* out, size_t out_size,
                        uint16_t* out_stride_bytes) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!out || !out_stride_bytes) return HOST_ERR_INVALID_ARG;
    const size_t bytes = s->surf->byteSize();
    if (out_size < bytes) return HOST_ERR_NO_MEMORY;
    std::memcpy(out, s->surf->buffer(), bytes);
    *out_stride_bytes = s->surf->strideBytes();
    return static_cast<int>(bytes);
}

int host_surface_export_jpg(uint32_t surface, uint8_t quality,
                            uint8_t* out, size_t out_size, uint32_t* out_len) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    if (!out || !out_len) return HOST_ERR_INVALID_ARG;
    size_t len = 0;
    int rc = cdc::image::encodeMonoJpeg(s->surf->buffer(),
                                        static_cast<uint16_t>(s->surf->width()),
                                        static_cast<uint16_t>(s->surf->height()),
                                        s->surf->strideBytes(), quality,
                                        out, out_size, &len);
    *out_len = static_cast<uint32_t>(len);
    switch (rc) {
        case 0:  return HOST_OK;
        case -3: return HOST_ERR_NO_MEMORY;
        case -2: return HOST_ERR_INVALID_ARG;
        default: return HOST_ERR_GENERIC;
    }
}

int host_surface_draw_sprite(uint32_t surface, int16_t x, int16_t y,
                             uint32_t sprite) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    auto* c = cdc::plugin_manager::PluginUiState::instance().canvasView();
    if (!c) return HOST_ERR_NOT_FOUND;
    cdc::ui::anim::SpriteStore::FrameView fv;
    if (!c->spriteFrameView(sprite, &fv)) return HOST_ERR_NOT_FOUND;
    render::BlitOpts opts;
    opts.mask   = fv.mask;
    opts.opaque = (fv.flags & cdc::ui::anim::SPRITE_FLAG_OPAQUE) != 0;
    opts.flipH  = (fv.flags & cdc::ui::anim::SPRITE_FLAG_FLIP_H) != 0;
    opts.flipV  = (fv.flags & cdc::ui::anim::SPRITE_FLAG_FLIP_V) != 0;
    opts.rot90  = (fv.flags & cdc::ui::anim::SPRITE_FLAG_ROT_90) != 0;
    opts.scale  = fv.scale;
    render::drawBitmapMasked(s->surf.get(), x, y, fv.data,
                             static_cast<int16_t>(fv.w),
                             static_cast<int16_t>(fv.h), opts, INK, 0);
    return HOST_OK;
}

int host_sprite_create_from_surface(uint32_t surface, uint16_t frame_w,
                                    uint16_t frame_h, uint16_t frame_count) {
    SurfaceSlot* s = find_slot(surface);
    if (!s) return HOST_ERR_NOT_FOUND;
    auto* c = cdc::plugin_manager::PluginUiState::instance().canvasView();
    if (!c) return HOST_ERR_NOT_FOUND;
    const uint16_t surfW = static_cast<uint16_t>(s->surf->width());
    const uint16_t surfH = static_cast<uint16_t>(s->surf->height());
    if (frame_w == 0 || frame_h == 0 || frame_count == 0
        || frame_w > surfW || frame_h > surfH) {
        return HOST_ERR_INVALID_ARG;
    }
    const uint16_t cellsPerRow = surfW / frame_w;
    const uint16_t rowsNeeded  = static_cast<uint16_t>(
        (frame_count + cellsPerRow - 1) / cellsPerRow);
    if (static_cast<uint32_t>(rowsNeeded) * frame_h > surfH) {
        return HOST_ERR_INVALID_ARG;
    }

    // Repack the grid cells into a vertically stacked sheet. Cells are not
    // byte-aligned in the surface, so this walks pixels.
    const uint16_t stride     = static_cast<uint16_t>((frame_w + 7) / 8);
    const uint32_t sheetBytes = static_cast<uint32_t>(stride) * frame_h * frame_count;
    auto sheet = std::make_unique<uint8_t[]>(sheetBytes);
    if (!sheet) return HOST_ERR_NO_MEMORY;
    std::memset(sheet.get(), 0, sheetBytes);

    const uint8_t* src        = s->surf->buffer();
    const uint16_t srcStride  = s->surf->strideBytes();
    for (uint16_t f = 0; f < frame_count; ++f) {
        const uint16_t cellX = static_cast<uint16_t>((f % cellsPerRow) * frame_w);
        const uint16_t cellY = static_cast<uint16_t>((f / cellsPerRow) * frame_h);
        uint8_t* dstFrame = sheet.get() + static_cast<uint32_t>(f) * stride * frame_h;
        for (uint16_t yy = 0; yy < frame_h; ++yy) {
            const uint8_t* srcRow = src + static_cast<uint32_t>(cellY + yy) * srcStride;
            uint8_t* dstRow = dstFrame + static_cast<uint32_t>(yy) * stride;
            for (uint16_t xx = 0; xx < frame_w; ++xx) {
                uint16_t sx = static_cast<uint16_t>(cellX + xx);
                if (srcRow[sx >> 3] & (0x80 >> (sx & 7))) {
                    dstRow[xx >> 3] |= static_cast<uint8_t>(0x80 >> (xx & 7));
                }
            }
        }
    }

    int32_t handle = c->spriteCreate(frame_w, frame_h, frame_count,
                                     sheet.get(), sheetBytes);
    if (handle > 0) return static_cast<int>(handle);
    return handle == 0 ? HOST_ERR_INVALID_ARG : HOST_ERR_NO_MEMORY;
}

// Free every surface owned by a plugin being unloaded.
void plg_surface_on_unload(void* plugin) {
    for (auto& s : s_slots) {
        if (s.surf && s.plugin == plugin) s = SurfaceSlot{};
    }
}

}  // extern "C"
