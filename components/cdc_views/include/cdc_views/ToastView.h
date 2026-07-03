#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * ToastView - Temporary overlay message
 *
 * Shows a centered message box with optional icon.
 * Auto-dismisses after timeout or on Y/N key press.
 *
 * Icons:
 *   NONE = No icon
 *   SUCCESS = Checkmark
 *   ERROR = X mark
 *   INFO = (i) icon
 */
class ToastView : public ViewBase {
public:
    enum class Icon : uint8_t {
        NONE = 0,
        SUCCESS,
        ERROR,
        INFO,
        TASK,
        ALERT
    };

    /**
     * Initialize toast with message and settings
     * @param message Text to display (copied internally)
     * @param icon Icon type
     * @param durationMs Display duration (0 = until dismissed)
     */
    void init(const char* message, Icon icon = Icon::NONE, uint16_t durationMs = 1500,
              bool dismissible = true);

    /**
     * Check if toast should be dismissed
     */
    bool isExpired() const { return expired_; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override { return "ToastView"; }

private:
    static constexpr uint16_t MAX_MSG_LEN = 64;
    static constexpr int BOX_WIDTH = 200;
    static constexpr int BOX_HEIGHT = 50;

    char message_[MAX_MSG_LEN] = {};
    Icon icon_ = Icon::NONE;
    uint16_t durationMs_ = 1500;
    uint32_t startMs_ = 0;
    bool started_ = false;
    bool expired_ = false;
    bool dismissible_ = true;
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a toast message (shown as modal overlay)
 * @param message Text to display
 * @param durationMs Duration in ms (default 1500)
 */
void showToast(const char* message, uint16_t durationMs = 1500);

/**
 * Show a success toast with checkmark icon
 */
void showToastSuccess(const char* message, uint16_t durationMs = 1500);

/**
 * Show an error toast with X icon
 */
void showToastError(const char* message, uint16_t durationMs = 1500);

/**
 * Show an info toast with (i) icon
 */
void showToastInfo(const char* message, uint16_t durationMs = 1500);

/**
 * Show a task toast with hourglass icon (default: until dismissed)
 */
void showToastTask(const char* message, uint16_t durationMs = 0);

/**
 * Show an alert toast with warning icon
 */
void showToastAlert(const char* message, uint16_t durationMs = 1500);

/**
 * Show a non-dismissible alert toast (panic)
 */
void showToastAlertSticky(const char* message);

} // namespace cdc::ui
