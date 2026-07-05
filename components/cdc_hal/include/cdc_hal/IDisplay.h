#pragma once

#include "cdc_core/IService.h"
#include <cstdint>

namespace cdc::hal {

/**
 * Display refresh modes
 */
enum class RefreshMode : uint8_t {
    FULL,          // Full display refresh (multi-flash OTP waveform, slow, no ghosting)
    FAST,          // Full-screen refresh with the short waveform (single flash); clears most ghosting.
                   // Escalated to FULL after FEATURE_EPD_MAX_FASTS_BEFORE_FULL fast refreshes.
    PARTIAL,       // Partial refresh (fast, may ghost); escalated to FAST after
                   // FEATURE_EPD_MAX_PARTIALS_BEFORE_FAST consecutive partials
    PARTIAL_LIGHT  // Partial refresh never promoted; for tiny low-churn updates (e.g. lock-screen clock)
};

/**
 * Display interface for E-Paper and other displays
 */
class IDisplay : public core::IService {
public:
    virtual ~IDisplay() = default;

    /**
     * Clear the display buffer
     */
    virtual void clear() = 0;

    /**
     * Flush buffer to display (async)
     * @param mode Refresh mode
     */
    virtual void flush(RefreshMode mode = RefreshMode::PARTIAL) = 0;

    /**
     * Flush buffer to display (blocking)
     * @param mode Refresh mode
     */
    virtual void flushSync(RefreshMode mode = RefreshMode::PARTIAL) = 0;

    /**
     * Check if display is busy (refreshing)
     */
    virtual bool isBusy() const = 0;

    /**
     * Get display dimensions
     */
    virtual uint16_t getWidth() const = 0;
    virtual uint16_t getHeight() const = 0;

    /**
     * Set backlight level (0-1023)
     * Note: E-Paper typically has frontlight, not backlight
     * Does NOT persist to NVS - call saveBacklight() to persist
     */
    virtual void setBacklight(uint16_t level) = 0;

    /**
     * Save current backlight level to NVS
     * Call this when the user confirms the brightness setting
     */
    virtual void saveBacklight() = 0;

    /**
     * Get current backlight level
     */
    virtual uint16_t getBacklight() const = 0;

    /**
     * Check if backlight is on (level > 0)
     */
    virtual bool isBacklightOn() const = 0;

    /**
     * Turn backlight on (restores saved level)
     */
    virtual void backlightOn() = 0;

    /**
     * Turn backlight off (preserves saved level)
     */
    virtual void backlightOff() = 0;

    /**
     * Get native display handle (for direct GFX access)
     * Returns Gdey029T94* for the CDC Badge
     */
    virtual void* getNativeHandle() = 0;

    /**
     * Show boot splash screen
     */
    virtual void showSplash(const char* subtitle = nullptr) = 0;

    // === GFX Drawing Methods (avoid unsafe casts) ===

    /**
     * Draw a single pixel
     */
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) = 0;

    /**
     * Draw a line
     */
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) = 0;

    /**
     * Draw a rectangle outline
     */
    virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;

    /**
     * Draw a filled rectangle
     */
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;

    /**
     * Set text cursor position
     */
    virtual void setCursor(int16_t x, int16_t y) = 0;

    /**
     * Set text color
     */
    virtual void setTextColor(uint16_t color) = 0;

    /**
     * Set text size multiplier
     */
    virtual void setTextSize(uint8_t size) = 0;

    /**
     * Set font (nullptr = built-in 6x8)
     */
    virtual void setFont(const void* font) = 0;

    /**
     * Print text at current cursor
     */
    virtual void print(const char* text) = 0;

    /**
     * Print formatted text
     */
    virtual void printf(const char* fmt, ...) = 0;
};

// Factory function to get display instance
IDisplay* getDisplayInstance();

/**
 * \brief Blink the backlight as a visual "look at me" signal.
 *
 * Toggles the e-paper frontlight off/on \p count times with \p period_ms
 * milliseconds in each half-cycle. Restores the original backlight state on
 * return. Safe to call from any FreeRTOS task: the underlying LEDC PWM API
 * is thread-safe and no framebuffer rendering happens.
 *
 * Used for FIDO2/CTAPHID WINK identification, USB pairing prompts, and any
 * other "draw the user's attention" notification.
 *
 * \param count Number of off/on cycles (clamped to 1..10, default 2).
 * \param period_ms Duration of each half-cycle in ms (clamped to 50..1000,
 *                  default 150). Total wall time is 2 * count * period_ms.
 */
void winkBacklight(uint8_t count = 2, uint16_t period_ms = 150);

} // namespace cdc::hal
