#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * DateInputView - Date input with day/month/year fields
 *
 * Navigation:
 *   0-9 = Enter digits
 *   4 = Previous field
 *   6 = Next field
 *   N = Clear current field / Cancel (if empty)
 *   Y = Confirm
 */
class DateInputView : public ViewBase {
public:
    /**
     * Confirm callback
     * @param day Day (1-31)
     * @param month Month (1-12)
     * @param year Year (e.g., 2026)
     */
    using ConfirmCallback = void(*)(uint8_t day, uint8_t month, uint16_t year);

    /**
     * Cancel callback (called when the user dismisses the view with N).
     * The view pops itself before this fires, so do not call pop() in the handler.
     */
    using CancelCallback = void(*)();

    /**
     * Initialize date input view
     * @param title View title
     * @param day Initial day (1-31)
     * @param month Initial month (1-12)
     * @param year Initial year
     */
    void init(const char* title, uint8_t day, uint8_t month, uint16_t year);

    /**
     * Set confirm callback
     */
    void setOnConfirm(ConfirmCallback callback) { onConfirm_ = callback; }

    /**
     * Set cancel callback (called when the view is dismissed without confirming)
     */
    void setOnCancel(CancelCallback callback) { onCancel_ = callback; }

    /**
     * Get current values
     */
    uint8_t getDay() const { return day_; }
    uint8_t getMonth() const { return month_; }
    uint16_t getYear() const { return year_; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    const char* getName() const override { return "DateInputView"; }
    const char* getFooterHint() const override;

private:
    enum class Field : uint8_t { DAY = 0, MONTH = 1, YEAR = 2 };

    const char* title_ = nullptr;
    uint8_t day_ = 1;
    uint8_t month_ = 1;
    uint16_t year_ = 2026;
    Field currentField_ = Field::DAY;
    uint8_t digitPos_ = 0;  // Position within current field
    ConfirmCallback onConfirm_ = nullptr;
    CancelCallback onCancel_ = nullptr;

    void nextField();
    void prevField();
    void clearField();
    void enterDigit(char digit);
    bool validateAndClamp();
};

} // namespace cdc::ui
