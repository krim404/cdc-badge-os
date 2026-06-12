#include "cdc_os_ui/SettingsHandlers.h"
#include "cdc_os_ui/views/LockScreenView.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/T9InputView.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IRtc.h"
#include "nvs.h"
#include <ctime>
#include <sys/time.h>

namespace cdc::ui::settings {

/**
 * \brief External dependencies injected by `AppUi`.
 */
static hal::IDisplay* s_display = nullptr;
static hal::ISleepController* s_sleep = nullptr;
static LockScreenView* s_lockScreen = nullptr;

/**
 * \brief Badge text editing workflow state.
 */
static constexpr uint8_t BADGE_STEP_NONE = 0;
static constexpr uint8_t BADGE_STEP_NAME = 1;
static constexpr uint8_t BADGE_STEP_INFO = 2;
static constexpr uint8_t BADGE_STEP_INFO2 = 3;
static uint8_t s_badgeTextPendingStep = BADGE_STEP_NONE;

/**
 * \brief Forward declarations for internal helper callbacks.
 */
static void showBadgeTextStep(uint8_t step);
static void onBadgeNameSave(const char* text);
static void onBadgeInfoSave(const char* text);
static void onBadgeInfo2Save(const char* text);

/**
 * \brief Initializes shared dependencies used by the settings handlers.
 * \param display Display service used for brightness and rendering-related settings.
 * \param sleep Sleep controller used for auto-sleep configuration.
 * \param lockScreen Lock screen view used to reflect updated badge text.
 * \return void
 */
void init(hal::IDisplay* display, hal::ISleepController* sleep, LockScreenView* lockScreen) {
    s_display = display;
    s_sleep = sleep;
    s_lockScreen = lockScreen;
}

/**
 * \brief Processes the next pending badge-text wizard step.
 * \return void
 */
void processPendingBadgeText() {
    if (s_badgeTextPendingStep == BADGE_STEP_NONE) return;
    uint8_t step = s_badgeTextPendingStep;
    s_badgeTextPendingStep = BADGE_STEP_NONE;
    showBadgeTextStep(step);
}

/**
 * \brief Persists and applies selected backlight value.
 * \param value Slider value in 0..10 scale.
 * \return void
 */
void onBrightnessSave(uint16_t value) {
    if (s_display) {
        s_display->setBacklight(value * 10);
        s_display->saveBacklight();
    }
}

/**
 * \brief Applies backlight preview without persisting.
 * \param value Slider value in 0..10 scale.
 * \return void
 */
void onBrightnessChange(uint16_t value) {
    if (s_display) {
        s_display->setBacklight(value * 10);
    }
}

/**
 * \brief Returns adaptive brightness step size.
 * \param current Current slider value.
 * \param increasing Direction flag.
 * \return Step size for next adjustment.
 */
uint16_t brightnessStepCallback(uint16_t current, bool increasing) {
    if (increasing) {
        if (current < 1) return 1;
        if (current < 20) return 5;
        return 10;
    }
    if (current <= 1) return 1;
    if (current <= 20) return 5;
    return 10;
}

/**
 * \brief Saves lock-screen sleep interval in minutes.
 * \param value Sleep interval in minutes.
 * \return void
 */
void onSleepIntervalSave(uint16_t value) {
    if (s_sleep) {
        s_sleep->setLightSleepInterval(static_cast<uint32_t>(value) * 60);
    }
}

/**
 * \brief Saves timezone offset and refreshes lock-screen clock.
 * \param value Slider value mapped to UTC offset.
 * \return void
 */
void onTimezoneSave(uint16_t value) {
    auto* rtc = hal::getRtcInstance();
    if (rtc) {
        // Value is 0-26 (slider range), convert to -12..+14 (actual timezone)
        int8_t tzOffset = static_cast<int8_t>(static_cast<int16_t>(value) - 12);
        rtc->setTimezoneOffset(tzOffset);

        // Update lock screen clock
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && s_lockScreen) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
            s_lockScreen->setClock(buf);
            snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
            s_lockScreen->setDate(buf);
        }
    }
}

/**
 * \brief Applies confirmed date to system time.
 * \param day Day value.
 * \param month Month value.
 * \param year Year value.
 * \return void
 */
void onDateConfirm(uint8_t day, uint8_t month, uint16_t year) {
    time_t now = time(nullptr);
    struct tm tm = {};
    struct tm* current = localtime(&now);
    if (current) tm = *current;
    tm.tm_mday = day;
    tm.tm_mon = month - 1;
    tm.tm_year = year - 1900;

    time_t newTime = mktime(&tm);
    struct timeval tv = {.tv_sec = newTime, .tv_usec = 0};
    settimeofday(&tv, nullptr);
}

/**
 * \brief Applies confirmed time to system clock.
 * \param hour Hour value.
 * \param minute Minute value.
 * \return void
 */
void onTimeConfirm(uint8_t hour, uint8_t minute) {
    time_t now = time(nullptr);
    struct tm tm = {};
    struct tm* current = localtime(&now);
    if (current) tm = *current;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;

    time_t newTime = mktime(&tm);
    struct timeval tv = {.tv_sec = newTime, .tv_usec = 0};
    settimeofday(&tv, nullptr);
}

/**
 * \brief Handles completion of PIN-change flow.
 * \param success Indicates whether PIN change succeeded.
 * \return void
 */
void onPinChangeComplete(bool success) {
    (void)success;
    ViewStack::instance().pop();
}

/**
 * \brief Starts badge-text editing wizard.
 * \return void
 */
void startBadgeTextEdit() {
    showBadgeTextStep(BADGE_STEP_NAME);
}

/**
 * \brief Shows one step of badge-text wizard.
 * \param step Wizard step identifier.
 * \return void
 */
static void showBadgeTextStep(uint8_t step) {
    if (!s_lockScreen) return;

    const char* title = nullptr;
    const char* initial = nullptr;
    T9InputView::SaveCallback cb = nullptr;

    switch (step) {
        case BADGE_STEP_NAME:
            title = ui::tr("core.name");
            initial = s_lockScreen->getDisplayName();
            cb = onBadgeNameSave;
            break;
        case BADGE_STEP_INFO:
            title = ui::tr("core.info");
            initial = s_lockScreen->getInfo();
            cb = onBadgeInfoSave;
            break;
        case BADGE_STEP_INFO2:
            title = ui::tr("core.info2");
            initial = s_lockScreen->getInfo2();
            cb = onBadgeInfo2Save;
            break;
        default:
            return;
    }

    showT9Input(title, initial, cb, LockScreenView::MAX_TEXT_LEN);
}

/**
 * \brief Handles save callback for badge display name.
 * \param text Saved text value.
 * \return void
 */
static void onBadgeNameSave(const char* text) {
    if (s_lockScreen) s_lockScreen->setDisplayName(text);
    saveDisplayField("name", text);
    s_badgeTextPendingStep = BADGE_STEP_INFO;
}

/**
 * \brief Handles save callback for badge info line 1.
 * \param text Saved text value.
 * \return void
 */
static void onBadgeInfoSave(const char* text) {
    if (s_lockScreen) s_lockScreen->setInfo(text);
    saveDisplayField("info", text);
    s_badgeTextPendingStep = BADGE_STEP_INFO2;
}

/**
 * \brief Handles save callback for badge info line 2.
 * \param text Saved text value.
 * \return void
 */
static void onBadgeInfo2Save(const char* text) {
    if (s_lockScreen) s_lockScreen->setInfo2(text);
    saveDisplayField("info2", text);
    s_badgeTextPendingStep = BADGE_STEP_NONE;
}

/**
 * \brief Saves one display text field to NVS.
 * \param key NVS key for the field.
 * \param value Field value to persist.
 * \return void
 */
void saveDisplayField(const char* key, const char* value) {
    if (!key) return;
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READWRITE, &nvs) != ESP_OK) return;
    const char* safeValue = value ? value : "";
    nvs_set_str(nvs, key, safeValue);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/**
 * \brief Reads one display text field from NVS into the caller buffer.
 * \param key NVS key for the field.
 * \param out Destination buffer.
 * \param outSize Capacity of \p out in bytes.
 * \return true if a non-empty value was read.
 */
bool loadDisplayField(const char* key, char* out, size_t outSize) {
    if (!key || !out || outSize == 0) return false;
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = outSize;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && len > 1;
}

} // namespace cdc::ui::settings
