#pragma once

#include <cstdint>
#include <cstddef>

// Forward declarations
namespace cdc::hal {
class IDisplay;
class ISleepController;
}
namespace cdc::ui {
class LockScreenView;
}

namespace cdc::ui {

// Settings-related callbacks and handlers
namespace settings {

// Initialize dependencies (call from AppUi::ui_init)
void init(hal::IDisplay* display, hal::ISleepController* sleep, LockScreenView* lockScreen);

// Process pending badge text steps (call from ui_process)
void processPendingBadgeText();

// Brightness control
void onBrightnessSave(uint16_t value);
void onBrightnessChange(uint16_t value);
uint16_t brightnessStepCallback(uint16_t current, bool increasing);

// Sleep interval
void onSleepIntervalSave(uint16_t value);

// Timezone
void onTimezoneSave(uint16_t value);

// Date/Time
void onDateConfirm(uint8_t day, uint8_t month, uint16_t year);
void onTimeConfirm(uint8_t hour, uint8_t minute);

// PIN change
void onPinChangeComplete(bool success);

// Badge text editing flow
void startBadgeTextEdit();

// NVS persistence for display fields
void saveDisplayField(const char* key, const char* value);

// Reads one display text field from NVS into the caller buffer.
// Returns true if a non-empty value was found; the buffer is left untouched
// otherwise.
bool loadDisplayField(const char* key, char* out, size_t outSize);

} // namespace settings

} // namespace cdc::ui
