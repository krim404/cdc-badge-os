#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

class Gdey029T94;

namespace cdc::ui {

/**
 * \brief Monochrome RGB color picker.
 *
 * Pick an RGB color via barycentric coordinates inside an R/G/B triangle.
 * A separate brightness slider scales the value. The chosen color is
 * previewed as a Bayer-4-dithered box reflecting perceived luminance
 * (Y = 0.299 R + 0.587 G + 0.114 B).
 *
 * Keys:
 *   2 / 4 / 6 / 8  move cursor inside triangle
 *   7 / 9          brightness -/+
 *   Y              save (fires callback with chosen 0..255 R/G/B)
 *   N              cancel (REQUEST_POP)
 */
class ColorPickerView : public ViewBase {
public:
    /// \brief Save callback. Values are pre-multiplied with brightness.
    using SaveCallback = void(*)(uint8_t r, uint8_t g, uint8_t b);

    /// \brief Cancel callback. The view pops itself before this fires.
    using CancelCallback = void(*)();

    /**
     * \brief Initialize the picker with a starting color.
     * \param r Initial red value (0..255).
     * \param g Initial green value (0..255).
     * \param b Initial blue value (0..255).
     */
    void init(uint8_t r, uint8_t g, uint8_t b);

    /// \brief Register a save callback (fired on Y).
    void setOnSave(SaveCallback cb) { onSave_ = cb; }

    /// \brief Register a cancel callback (fired when dismissed with N).
    void setOnCancel(CancelCallback cb) { onCancel_ = cb; }

    /// \brief Read the currently chosen color (post brightness scaling).
    void currentColor(uint8_t& r, uint8_t& g, uint8_t& b) const;

    // IView
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override { return "ColorPickerView"; }
    const char* getFooterHint() const override;

private:
    SaveCallback onSave_ = nullptr;
    CancelCallback onCancel_ = nullptr;

    int16_t cursorX_ = 0;
    int16_t cursorY_ = 0;
    uint8_t value_   = 255;

    uint32_t repeatStartMs_ = 0;
    uint32_t lastRepeatMs_  = 0;

    void barycentric(int16_t px, int16_t py,
                     int32_t& aR, int32_t& aG, int32_t& aB) const;
    void clampInsideTriangle();
    void setFromRGB(uint8_t r, uint8_t g, uint8_t b);
    void moveCursor(int16_t dx, int16_t dy);
    void adjustValue(int8_t delta);
    void rawColor(uint8_t& r, uint8_t& g, uint8_t& b) const;
    void drawDitheredBox(Gdey029T94* gfx, int16_t x, int16_t y, int16_t w, int16_t h,
                         uint8_t luminance) const;
    void drawTriangle(Gdey029T94* gfx, int16_t ox, int16_t oy) const;
};

} // namespace cdc::ui
