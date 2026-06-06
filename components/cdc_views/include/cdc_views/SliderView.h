#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * SliderView - Value adjustment with visual bar
 *
 * Displays a slider bar with current value.
 * Supports min/max/step configuration.
 *
 * Keys:
 *   4 = Decrease (left)
 *   6 = Increase (right)
 *   Y = Save (triggers callback)
 *   N = Cancel (REQUEST_POP)
 */
class SliderView : public ViewBase {
public:
    /**
     * Save callback (called when Y is pressed)
     * @param value Final value
     */
    using SaveCallback = void(*)(uint16_t value);

    /**
     * Change callback (called on every value change for real-time updates)
     * @param value Current value
     */
    using ChangeCallback = void(*)(uint16_t value);

    /**
     * Dynamic step callback (returns step size based on current value)
     * @param currentValue Current slider value
     * @param increasing True if increasing, false if decreasing
     * @return Step size to use
     */
    using StepCallback = uint16_t(*)(uint16_t currentValue, bool increasing);

    /**
     * Cancel callback (called when the user dismisses the view with N).
     * The view pops itself before this fires, so do not call pop() in the handler.
     */
    using CancelCallback = void(*)();

    /**
     * Initialize slider view
     * @param title Slider title
     * @param minVal Minimum value
     * @param maxVal Maximum value
     * @param initial Initial value
     * @param step Step size for adjustments
     * @param unit Unit string (e.g., "%", "min")
     */
    void init(const char* title, uint16_t minVal, uint16_t maxVal,
              uint16_t initial, uint16_t step, const char* unit = nullptr);

    /**
     * Set save callback (called on Y key)
     */
    void setOnSave(SaveCallback callback) { onSave_ = callback; }

    /**
     * Set change callback (called on every value adjustment for real-time updates)
     */
    void setOnChange(ChangeCallback callback) { onChange_ = callback; }

    /**
     * Set dynamic step callback (overrides fixed step size)
     * For example: brightness uses smaller steps at low values
     */
    void setStepCallback(StepCallback callback) { stepCallback_ = callback; }

    /**
     * Set cancel callback (called when the view is dismissed without saving)
     */
    void setOnCancel(CancelCallback callback) { onCancel_ = callback; }

    /**
     * Get current value
     */
    uint16_t getValue() const { return value_; }

    /**
     * Set current value
     */
    void setValue(uint16_t value);

    /**
     * Set display offset (added to value for display only)
     * Useful when internal value differs from displayed value
     */
    void setDisplayOffset(int16_t offset) { displayOffset_ = offset; }

    /**
     * Set special label for zero value (e.g., "Never" instead of "0")
     */
    void setZeroLabel(const char* label) { zeroLabel_ = label; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override { return "SliderView"; }
    const char* getFooterHint() const override;

private:
    const char* title_ = nullptr;
    const char* unit_ = nullptr;
    const char* zeroLabel_ = nullptr;
    uint16_t value_ = 0;
    uint16_t minValue_ = 0;
    uint16_t maxValue_ = 100;
    uint16_t step_ = 1;
    int16_t displayOffset_ = 0;
    SaveCallback onSave_ = nullptr;
    CancelCallback onCancel_ = nullptr;
    ChangeCallback onChange_ = nullptr;
    StepCallback stepCallback_ = nullptr;
    uint32_t repeatStartMs_ = 0;
    uint32_t lastRepeatMs_ = 0;

    void adjust(bool increase);
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a slider and push it to the ViewStack.
 * Simplest possible API for value adjustment.
 *
 * @param title Slider title
 * @param minVal Minimum value
 * @param maxVal Maximum value
 * @param initial Initial value
 * @param step Step size
 * @param unit Unit string (e.g., "%")
 * @param onSave Called when Y is pressed (save)
 * @param onChange Called on every adjustment (real-time preview)
 * @return Pointer to the SliderView (for further configuration)
 *
 * Example (Brightness):
 *   showSlider("Brightness", 0, 100, 50, 10, "%",
 *              [](uint16_t v) { saveNVS(v); },
 *              [](uint16_t v) { display->setBacklight(v * 10); });
 */
SliderView* showSlider(const char* title, uint16_t minVal, uint16_t maxVal,
                       uint16_t initial, uint16_t step, const char* unit,
                       SliderView::SaveCallback onSave,
                       SliderView::ChangeCallback onChange = nullptr);

} // namespace cdc::ui
