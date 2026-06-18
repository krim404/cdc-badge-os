#pragma once

#include <cstddef>
#include <cstdint>

#include "cdc_core/Raii.h"
#include "cdc_image/MonoBitmap.h"
#include "cdc_ui/IView.h"

namespace cdc::ui {

/**
 * \brief Full-screen viewer for a decoded PNG/JPEG, dithered to the e-paper.
 *
 * Decodes on \ref init into two 1-bit bitmaps held in PSRAM: one scaled to fit
 * the screen (default view) and one at native resolution for an actual-size pan
 * mode. Decode failures show a readable message instead of crashing.
 *
 * Keys: 5 = toggle fit/actual, 2/4/6/8 = pan (actual mode), N = back.
 */
class ImageView : public ViewBase {
public:
    static constexpr uint32_t MAX_PIXELS = 1048576;  ///< ~1 megapixel cap.

    /**
     * \brief Decodes and lays out an image.
     * \param title Header/footer title (CP437 file name).
     * \param data Encoded image bytes (PNG or JPEG).
     * \param len Byte length.
     */
    void init(const char* title, const uint8_t* data, size_t len);

    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onExit() override;
    const char* getName() const override { return "ImageView"; }
    const char* getFooterHint() const override;

private:
    enum class Mode : uint8_t { Fit, Actual };
    static constexpr uint16_t MAX_TITLE_LEN = 64;
    static constexpr int PAN_STEP = 24;

    char titleBuf_[MAX_TITLE_LEN];
    const char* errorKey_ = nullptr;
    cdc::core::PsramUniquePtr<uint8_t> fitBits_;
    cdc::core::PsramUniquePtr<uint8_t> fullBits_;
    cdc::image::MonoBitmap fitBmp_;
    cdc::image::MonoBitmap fullBmp_;
    Mode mode_ = Mode::Fit;
    int panX_ = 0;
    int panY_ = 0;

    void clampPan();
};

/**
 * \brief Decodes and shows an image on the view stack.
 * \param title File name / title.
 * \param data Encoded image bytes.
 * \param len Byte length.
 * \return Pointer to the shared ImageView instance.
 */
ImageView* showImage(const char* title, const uint8_t* data, size_t len);

} // namespace cdc::ui
