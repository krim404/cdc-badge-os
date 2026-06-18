/**
 * ImageView Implementation
 *
 * Decodes a PNG/JPEG to two 1-bit bitmaps (fit + native) and renders them on
 * the e-paper with a pan mode for actual size.
 */

#include "cdc_views/ImageView.h"

#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"

#include "cdc_image/Dither.h"
#include "cdc_image/ImageDecoder.h"

#include <goodisplay/gdey029T94.h>

#include <cstring>

static const char* TAG = "ImageView";

using cdc::ui::layout::FOOTER_HEIGHT;

namespace cdc::ui {

void ImageView::init(const char* title, const uint8_t* data, size_t len) {
    if (title) {
        strncpy(titleBuf_, title, MAX_TITLE_LEN - 1);
        titleBuf_[MAX_TITLE_LEN - 1] = '\0';
    } else {
        titleBuf_[0] = '\0';
    }

    errorKey_ = nullptr;
    fitBits_.reset();
    fullBits_.reset();
    fitBmp_ = {};
    fullBmp_ = {};
    mode_ = Mode::Fit;
    panX_ = 0;
    panY_ = 0;
    dirty_ = true;

    uint16_t w = 0, h = 0;
    const char* err = nullptr;
    auto gray = cdc::image::decodeToGray(data, len, MAX_PIXELS, w, h, err);
    if (!gray) {
        errorKey_ = err ? err : "core.img_decode_failed";
        return;
    }

    auto scratch = cdc::core::psramAlloc<int16_t>(cdc::image::Ditherer::scratchCells(w));
    if (!scratch) {
        errorKey_ = "core.img_decode_failed";
        return;
    }

    // Native-resolution bitmap (actual-size / pan).
    fullBits_ = cdc::core::psramAlloc<uint8_t>(cdc::image::MonoBitmap::byteSize(w, h));
    if (fullBits_) {
        fullBmp_.width = w;
        fullBmp_.height = h;
        fullBmp_.stride = cdc::image::MonoBitmap::strideFor(w);
        fullBmp_.bits = fullBits_.get();
        cdc::image::ditherImage(gray.get(), w, h, fullBmp_.bits, scratch.get());
    }

    // Fit-to-screen bitmap.
    hal::IDisplay* display = hal::getDisplayInstance();
    const uint16_t scrW = display ? display->getWidth() : 296;
    const uint16_t scrH = display ? display->getHeight() : 128;
    const uint16_t areaH = static_cast<uint16_t>(scrH - FOOTER_HEIGHT);

    uint16_t fw = w, fh = h;
    if (w > scrW || h > areaH) {
        const uint32_t sx = (static_cast<uint32_t>(scrW) * 1000) / w;
        const uint32_t sy = (static_cast<uint32_t>(areaH) * 1000) / h;
        const uint32_t s = sx < sy ? sx : sy;
        fw = static_cast<uint16_t>((static_cast<uint32_t>(w) * s) / 1000);
        fh = static_cast<uint16_t>((static_cast<uint32_t>(h) * s) / 1000);
        if (fw == 0) fw = 1;
        if (fh == 0) fh = 1;
    }

    fitBits_ = cdc::core::psramAlloc<uint8_t>(cdc::image::MonoBitmap::byteSize(fw, fh));
    auto fitGray = cdc::core::psramAlloc<uint8_t>(static_cast<size_t>(fw) * fh);
    if (fitBits_ && fitGray) {
        fitBmp_.width = fw;
        fitBmp_.height = fh;
        fitBmp_.stride = cdc::image::MonoBitmap::strideFor(fw);
        fitBmp_.bits = fitBits_.get();
        cdc::image::downscaleGrayBox(gray.get(), w, h, fitGray.get(), fw, fh);
        cdc::image::ditherImage(fitGray.get(), fw, fh, fitBmp_.bits, scratch.get());
    }

    LOG_D(TAG, "init: %ux%u -> fit %ux%u", w, h, fw, fh);
    // gray, fitGray, scratch released on scope exit.
}

void ImageView::onExit() {
    fitBits_.reset();
    fullBits_.reset();
    fitBmp_ = {};
    fullBmp_ = {};
    errorKey_ = nullptr;
}

void ImageView::clampPan() {
    hal::IDisplay* display = hal::getDisplayInstance();
    const int scrW = display ? display->getWidth() : 296;
    const int scrH = display ? display->getHeight() : 128;
    const int areaH = scrH - FOOTER_HEIGHT;
    const int maxX = fullBmp_.width > scrW ? fullBmp_.width - scrW : 0;
    const int maxY = fullBmp_.height > areaH ? fullBmp_.height - areaH : 0;
    if (panX_ < 0) panX_ = 0;
    if (panY_ < 0) panY_ = 0;
    if (panX_ > maxX) panX_ = maxX;
    if (panY_ > maxY) panY_ = maxY;
}

const char* ImageView::getFooterHint() const {
    return ui::tr("core.hint_image");
}

InputResult ImageView::onKey(char key) {
    if (errorKey_) {
        return key == KEY_NO ? InputResult::REQUEST_POP : InputResult::IGNORED;
    }
    switch (key) {
        case KEY_NO:
            return InputResult::REQUEST_POP;
        case KEY_SELECT:
            mode_ = (mode_ == Mode::Fit) ? Mode::Actual : Mode::Fit;
            panX_ = 0;
            panY_ = 0;
            dirty_ = true;
            return InputResult::CONSUMED;
        default:
            break;
    }
    if (mode_ == Mode::Actual) {
        switch (key) {
            case KEY_UP:    panY_ -= PAN_STEP; clampPan(); dirty_ = true; return InputResult::CONSUMED;
            case KEY_DOWN:  panY_ += PAN_STEP; clampPan(); dirty_ = true; return InputResult::CONSUMED;
            case '4':       panX_ -= PAN_STEP; clampPan(); dirty_ = true; return InputResult::CONSUMED;
            case '6':       panX_ += PAN_STEP; clampPan(); dirty_ = true; return InputResult::CONSUMED;
            default:        break;
        }
    }
    return InputResult::IGNORED;
}

void ImageView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();
    const int areaBottom = height - FOOTER_HEIGHT;

    gfx->fillScreen(EPD_WHITE);
    gfx->setFont(nullptr);
    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    if (errorKey_) {
        gfx->setCursor(8, areaBottom / 2);
        render::printText(gfx, ui::tr(errorKey_));
        render::drawFooterBar(gfx, width, height, titleBuf_[0] ? titleBuf_ : nullptr,
                              getFooterHint(), true);
        dirty_ = false;
        return;
    }

    if (mode_ == Mode::Fit && fitBmp_.bits) {
        int x = (static_cast<int>(width) - fitBmp_.width) / 2;
        int y = (areaBottom - fitBmp_.height) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        gfx->drawBitmap(x, y, fitBmp_.bits, fitBmp_.width, fitBmp_.height, EPD_BLACK);
    } else if (fullBmp_.bits) {
        gfx->drawBitmap(-panX_, -panY_, fullBmp_.bits, fullBmp_.width, fullBmp_.height, EPD_BLACK);
    }

    render::drawFooterBar(gfx, width, height, titleBuf_[0] ? titleBuf_ : nullptr,
                          getFooterHint(), true);
    dirty_ = false;
}

static ImageView s_sharedImageView;

ImageView* showImage(const char* title, const uint8_t* data, size_t len) {
    s_sharedImageView.init(title, data, len);
    ViewStack::instance().push(&s_sharedImageView);
    return &s_sharedImageView;
}

} // namespace cdc::ui
