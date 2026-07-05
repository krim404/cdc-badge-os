/**
 * E-Paper Display HAL Implementation
 * Wraps CalEPD library for Gdey029T94 (296x128 B/W)
 *
 * WICHTIG: Alle Objekte werden LAZY initialisiert um Crashes
 * durch globale Konstruktoren zu vermeiden!
 */

#include "cdc_hal/IDisplay.h"
#include "cdc_hal/hw_config.h"
#include "cdc_core/Raii.h"
#include "cdc_core/feature_flags.h"
#include "cdc_log.h"
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <goodisplay/gdey029T94.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <cstring>
#include <cstdarg>

static const char* TAG = "EpaperDisplay";

/** \brief Splash-screen text defaults. */
static constexpr const char* SPLASH_TITLE = "CDC Badge";
static constexpr const char* SPLASH_VERSION = "v" APP_VERSION;

namespace cdc::hal {

/** \brief LEDC backlight PWM configuration constants. */
static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;
static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_timer_bit_t LEDC_DUTY_RES = LEDC_TIMER_10_BIT;
static constexpr uint32_t LEDC_FREQUENCY = 10000;

/** \brief NVS namespace and keys for display settings. */
static constexpr const char* NVS_NAMESPACE = "display";
static constexpr const char* NVS_KEY_BACKLIGHT = "backlight";

/** \brief Display timing and geometry constants. */
static constexpr uint16_t WIDTH = 296;
static constexpr uint16_t HEIGHT = 128;
static constexpr uint16_t BACKLIGHT_DEFAULT = 512;
static constexpr uint16_t BACKLIGHT_MAX = 1023;

/** \brief Lazily initialized display objects to avoid global constructors. */
static EpdSpi* s_epd_spi = nullptr;
static Gdey029T94* s_epd_display = nullptr;

/** \brief Mutable display state cache. */
static bool s_initialized = false;
static uint16_t s_backlightLevel = BACKLIGHT_DEFAULT;
static bool s_backlightOn = true;

// Serialises writes to the backlight state (s_backlightOn / s_backlightLevel)
// and the LEDC duty across the UI, fido2 and ctaphid tasks.
static SemaphoreHandle_t s_backlightMutex = nullptr;

/** \brief Render-task runtime state. */
static SemaphoreHandle_t s_renderMutex = nullptr;
static TaskHandle_t s_renderTask = nullptr;
static volatile bool s_renderPending = false;
static volatile RefreshMode s_renderMode = RefreshMode::PARTIAL;

// Serialises the actual SSD1680 SPI transfer. The render task and any
// synchronous flushSync() caller (e.g. a plugin host edit running on the
// plg_tick task) may both drive the panel; the driver is not reentrant.
static SemaphoreHandle_t s_panelMutex = nullptr;

// The SSD1680 accumulates ghosting across consecutive partial updates and
// eventually stops applying new partials cleanly. Refreshes escalate in two
// stages to bound ghosting with minimal visible flashing: after
// FEATURE_EPD_MAX_PARTIALS_BEFORE_FAST partials the next refresh is promoted
// to FAST (single flash), and after FEATURE_EPD_MAX_FASTS_BEFORE_FULL fast
// refreshes the next one is promoted to FULL (multi-flash OTP waveform).
// Both counters are guarded by s_panelMutex.
static_assert(FEATURE_EPD_MAX_PARTIALS_BEFORE_FAST > 0, "partial threshold must be positive");
static_assert(FEATURE_EPD_MAX_FASTS_BEFORE_FULL > 0, "fast threshold must be positive");
static uint16_t s_partialsSinceFast = 0;
static uint16_t s_fastsSinceFull = 0;
static constexpr uint16_t kMaxPartialsBeforeFast = FEATURE_EPD_MAX_PARTIALS_BEFORE_FAST;
static constexpr uint16_t kMaxFastsBeforeFull = FEATURE_EPD_MAX_FASTS_BEFORE_FULL;

// Decide the effective refresh mode, escalating PARTIAL -> FAST -> FULL to
// clear ghosting. Caller must hold s_panelMutex.
static RefreshMode resolveRefresh(RefreshMode mode) {
    // Light partials (e.g. the lock-screen clock) never promote and do not
    // advance the ghost counters; they stay partial until a FAST/FULL clears them.
    if (mode == RefreshMode::PARTIAL_LIGHT) {
        return mode;
    }
    if (mode == RefreshMode::PARTIAL) {
        if (s_partialsSinceFast < kMaxPartialsBeforeFast) {
            ++s_partialsSinceFast;
            return mode;
        }
        mode = RefreshMode::FAST; // falls through to the FAST accounting below
    }
    if (mode == RefreshMode::FAST) {
        s_partialsSinceFast = 0;
        if (s_fastsSinceFull < kMaxFastsBeforeFull) {
            // A FAST refresh does not fully DC-balance the film, so it does
            // not reset the full-refresh counter.
            ++s_fastsSinceFull;
#if FEATURE_EPD_FAST_REFRESH
            return mode;
#else
            return RefreshMode::FULL; // hardware fast waveform disabled
#endif
        }
        mode = RefreshMode::FULL;
    }
    s_partialsSinceFast = 0;
    s_fastsSinceFull = 0;
    return RefreshMode::FULL;
}

// Coalescing rank for queued async refreshes: when several flush() calls
// collapse into one render, the stronger mode wins.
// FULL > FAST > PARTIAL > PARTIAL_LIGHT.
static int refreshStrength(RefreshMode mode) {
    switch (mode) {
        case RefreshMode::FULL:          return 3;
        case RefreshMode::FAST:          return 2;
        case RefreshMode::PARTIAL:       return 1;
        case RefreshMode::PARTIAL_LIGHT: return 0;
    }
    return 1;
}

// Drive the panel with the already-resolved refresh mode. Caller must hold
// s_panelMutex.
static void driveRefresh(RefreshMode resolved) {
    switch (resolved) {
        case RefreshMode::FULL:
            s_epd_display->update();
            break;
        case RefreshMode::FAST:
            s_epd_display->updateFast();
            break;
        default:
            // updateWindow takes physical coordinates (128 x 296). HAL
            // WIDTH/HEIGHT are logical post-rotation values; swap them and
            // pass using_rotation=false.
            s_epd_display->updateWindow(0, 0, HEIGHT, WIDTH, false);
            break;
    }
}

/**
 * \brief Applies backlight PWM duty level.
 * \param level Duty value in LEDC resolution units.
 */
static void applyBacklight(uint16_t level) {
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, level);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/**
 * \brief Loads persisted backlight level from NVS.
 */
static void loadBacklight() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t saved = BACKLIGHT_DEFAULT;
        if (nvs_get_u16(nvs, NVS_KEY_BACKLIGHT, &saved) == ESP_OK) {
            s_backlightLevel = (saved > BACKLIGHT_MAX) ? BACKLIGHT_MAX : saved;
        }
        nvs_close(nvs);
    }
}

/**
 * \brief Persists backlight level to NVS.
 * \param level Backlight level to store.
 */
static void persistBacklight(uint16_t level) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u16(nvs, NVS_KEY_BACKLIGHT, level);
        nvs_commit(nvs);
        nvs_close(nvs);
        LOG_D(TAG, "Backlight saved to NVS: %u", level);
    }
}

/**
 * \brief Render worker task processing async flush requests.
 * \param arg Task parameter (unused).
 */
static void renderTask(void* arg) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        RefreshMode mode;
        {
            cdc::core::MutexGuard guard(s_renderMutex);
            mode = s_renderMode;
            s_renderPending = false;
        }

        if (s_epd_display) {
            cdc::core::MutexGuard guard(s_panelMutex);
            driveRefresh(resolveRefresh(mode));
        }
    }
}

/**
 * Simple display wrapper using static functions
 */
class EpaperDisplay : public IDisplay {
public:
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "display"; }

    void clear() override;
    void flush(RefreshMode mode) override;
    void flushSync(RefreshMode mode) override;
    bool isBusy() const override { return s_renderPending; }
    uint16_t getWidth() const override { return WIDTH; }
    uint16_t getHeight() const override { return HEIGHT; }
    void setBacklight(uint16_t level) override;
    void saveBacklight() override;
    uint16_t getBacklight() const override { return s_backlightLevel; }
    bool isBacklightOn() const override { return s_backlightOn && s_backlightLevel > 0; }
    void backlightOn() override;
    void backlightOff() override;
    void* getNativeHandle() override { return s_epd_display; }
    void showSplash(const char* subtitle = nullptr) override;

    // GFX Drawing Methods
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void setCursor(int16_t x, int16_t y) override;
    void setTextColor(uint16_t color) override;
    void setTextSize(uint8_t size) override;
    void setFont(const void* font) override;
    void print(const char* text) override;
    void printf(const char* fmt, ...) override;

private:
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

/**
 * \brief Initializes display hardware, backlight, and render task.
 * \return `true` on successful initialization.
 */
bool EpaperDisplay::init() {
    if (s_initialized) {
        return true;
    }

    LOG_I(TAG, "Initializing E-Paper display...");

    // Load backlight from NVS
    loadBacklight();

    // Configure backlight PWM
    ledc_timer_config_t ledcTimer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_USE_XTAL_CLK,
        .deconfigure = false
    };
    ledc_timer_config(&ledcTimer);

    ledc_channel_config_t ledcChannel = {
        .gpio_num = EPD_LED_PIN,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = s_backlightLevel,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags = {.output_invert = 0}
    };
    ledc_channel_config(&ledcChannel);

    LOG_I(TAG, "Backlight configured");

    // LAZY create display objects
    if (!s_epd_spi) {
        s_epd_spi = new EpdSpi();
    }
    if (!s_epd_display) {
        s_epd_display = new Gdey029T94(*s_epd_spi);
    }

    // Initialize display
    s_epd_display->init(false);
    s_epd_display->setRotation(1);
    s_epd_display->setMonoMode(true);
    s_epd_display->cp437(true);
    s_epd_display->fillScreen(EPD_WHITE);

    LOG_I(TAG, "Display hardware initialized");

    // Create render task
    s_renderMutex = xSemaphoreCreateMutex();
    s_panelMutex = xSemaphoreCreateMutex();
    s_backlightMutex = xSemaphoreCreateMutex();
    if (!s_renderMutex || !s_panelMutex || !s_backlightMutex) {
        LOG_E(TAG, "Failed to create render mutex");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    BaseType_t ret = xTaskCreate(renderTask, "epd_render", 4096, nullptr, 5, &s_renderTask);
    if (ret != pdPASS) {
        LOG_E(TAG, "Failed to create render task");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    s_initialized = true;
    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "Display initialized (%ux%u), backlight=%u", WIDTH, HEIGHT, s_backlightLevel);
    return true;
}

/**
 * \brief Starts display service and enables backlight.
 * \return `true` if display is started after the call.
 */
bool EpaperDisplay::start() {
    if (state_ == core::ServiceState::INITIALIZED) {
        state_ = core::ServiceState::STARTED;
        s_backlightOn = true;
        applyBacklight(s_backlightLevel);
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops display service and disables backlight.
 */
void EpaperDisplay::stop() {
    if (state_ == core::ServiceState::STARTED) {
        s_backlightOn = false;
        applyBacklight(0);
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Clears framebuffer to white.
 */
void EpaperDisplay::clear() {
    if (s_epd_display) {
        s_epd_display->fillScreen(EPD_WHITE);
    }
}

/**
 * \brief Requests asynchronous display refresh.
 * \param mode Refresh mode.
 */
void EpaperDisplay::flush(RefreshMode mode) {
    // If no render task, fall back to sync
    if (!s_renderTask) {
        flushSync(mode);
        return;
    }

    {
        cdc::core::MutexGuard guard(s_renderMutex);
        if (s_renderPending) {
            // Coalesce with the already-queued refresh: keep the stronger mode.
            if (refreshStrength(mode) > refreshStrength(s_renderMode)) s_renderMode = mode;
        } else {
            s_renderPending = true;
            s_renderMode = mode;
        }
    }

    xTaskNotifyGive(s_renderTask);
}

/**
 * \brief Performs synchronous display refresh.
 * \param mode Refresh mode.
 */
void EpaperDisplay::flushSync(RefreshMode mode) {
    if (!s_epd_display) return;
    cdc::core::MutexGuard guard(s_panelMutex);
    driveRefresh(resolveRefresh(mode));
}

/**
 * \brief Sets current backlight level and applies immediately.
 * \param level New backlight level.
 */
void EpaperDisplay::setBacklight(uint16_t level) {
    if (level > BACKLIGHT_MAX) level = BACKLIGHT_MAX;
    cdc::core::MutexGuard guard(s_backlightMutex);
    s_backlightLevel = level;
    // Always apply immediately for live preview (e.g., brightness slider)
    // Also turn on backlight if level > 0
    if (level > 0) {
        s_backlightOn = true;
    }
    applyBacklight(level);
}

/**
 * \brief Persists current backlight level.
 */
void EpaperDisplay::saveBacklight() {
    persistBacklight(s_backlightLevel);
}

/**
 * \brief Enables backlight using current configured level.
 */
void EpaperDisplay::backlightOn() {
    cdc::core::MutexGuard guard(s_backlightMutex);
    s_backlightOn = true;
    applyBacklight(s_backlightLevel);
    LOG_I(TAG, "Backlight ON (level=%u)", s_backlightLevel);
}

/**
 * \brief Disables backlight output.
 */
void EpaperDisplay::backlightOff() {
    cdc::core::MutexGuard guard(s_backlightMutex);
    s_backlightOn = false;
    applyBacklight(0);
    LOG_I(TAG, "Backlight OFF");
}

/**
 * \brief Renders and displays boot splash screen.
 * \param subtitle Optional subtitle override.
 */
void EpaperDisplay::showSplash(const char* subtitle) {
    if (!s_epd_display) return;

    LOG_I(TAG, "Showing splash screen");

    s_epd_display->fillScreen(EPD_BLACK);
    s_epd_display->setTextColor(EPD_WHITE);

    // App name - large, centered
    s_epd_display->setFont(&FreeMonoBold12pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    s_epd_display->getTextBounds(SPLASH_TITLE, 0, 0, &x1, &y1, &w, &h);
    int name_x = (s_epd_display->width() - w) / 2;
    s_epd_display->setCursor(name_x, 55);
    s_epd_display->print(SPLASH_TITLE);

    // Subtitle or version - smaller, centered below name
    const char* sub = subtitle ? subtitle : SPLASH_VERSION;
    s_epd_display->setFont(&FreeMonoBold9pt7b);
    s_epd_display->getTextBounds(sub, 0, 0, &x1, &y1, &w, &h);
    int ver_x = (s_epd_display->width() - w) / 2;
    s_epd_display->setCursor(ver_x, 80);
    s_epd_display->print(sub);

    // Small text at bottom - built-in font (6x8)
    s_epd_display->setFont(nullptr);
    s_epd_display->setTextSize(1);

    // Build date
    char build_str[20];
    snprintf(build_str, sizeof(build_str), "%.3s%2.2s %.5s", __DATE__, __DATE__ + 4, __TIME__);
    s_epd_display->setCursor(2, 120);
    s_epd_display->print(build_str);

    // Right: tagline
    const char* status_text = "Open Hardware Security";
    int status_x = s_epd_display->width() - (strlen(status_text) * 6) - 2;
    s_epd_display->setCursor(status_x, 120);
    s_epd_display->print(status_text);

    // Full refresh (blocking)
    s_epd_display->update();
    LOG_I(TAG, "Splash screen displayed");
}

/** \brief Adafruit-GFX method implementations. */

/**
 * \brief Draws a single pixel on framebuffer.
 * \param x X coordinate.
 * \param y Y coordinate.
 * \param color Pixel color.
 */
void EpaperDisplay::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (s_epd_display) s_epd_display->drawPixel(x, y, color);
}

/**
 * \brief Draws a line on framebuffer.
 * \param x0 Start x.
 * \param y0 Start y.
 * \param x1 End x.
 * \param y1 End y.
 * \param color Line color.
 */
void EpaperDisplay::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    if (s_epd_display) s_epd_display->drawLine(x0, y0, x1, y1, color);
}

/**
 * \brief Draws rectangle outline on framebuffer.
 * \param x Left coordinate.
 * \param y Top coordinate.
 * \param w Width.
 * \param h Height.
 * \param color Outline color.
 */
void EpaperDisplay::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (s_epd_display) s_epd_display->drawRect(x, y, w, h, color);
}

/**
 * \brief Draws filled rectangle on framebuffer.
 * \param x Left coordinate.
 * \param y Top coordinate.
 * \param w Width.
 * \param h Height.
 * \param color Fill color.
 */
void EpaperDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (s_epd_display) s_epd_display->fillRect(x, y, w, h, color);
}

/**
 * \brief Sets text cursor position.
 * \param x Cursor x.
 * \param y Cursor y.
 */
void EpaperDisplay::setCursor(int16_t x, int16_t y) {
    if (s_epd_display) s_epd_display->setCursor(x, y);
}

/**
 * \brief Sets active text color.
 * \param color Text color.
 */
void EpaperDisplay::setTextColor(uint16_t color) {
    if (s_epd_display) s_epd_display->setTextColor(color);
}

/**
 * \brief Sets active text scale.
 * \param size Text size factor.
 */
void EpaperDisplay::setTextSize(uint8_t size) {
    if (s_epd_display) s_epd_display->setTextSize(size);
}

/**
 * \brief Sets active font pointer.
 * \param font Font pointer cast-compatible with `GFXfont`.
 */
void EpaperDisplay::setFont(const void* font) {
    if (s_epd_display) s_epd_display->setFont(static_cast<const GFXfont*>(font));
}

/**
 * \brief Prints text at current cursor position.
 * \param text Null-terminated string.
 */
void EpaperDisplay::print(const char* text) {
    if (s_epd_display && text) s_epd_display->print(text);
}

/**
 * \brief Formatted print helper for display text output.
 * \param fmt Printf-style format string.
 */
void EpaperDisplay::printf(const char* fmt, ...) {
    if (!s_epd_display || !fmt) return;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_epd_display->print(buf);
}

/** \brief Lazily created singleton display instance. */
static EpaperDisplay* s_display = nullptr;

/**
 * \brief Returns lazily created singleton display instance.
 * \return Pointer to global display service.
 */
IDisplay* getDisplayInstance() {
    if (!s_display) {
        s_display = new EpaperDisplay();
    }
    return s_display;
}

void winkBacklight(uint8_t count, uint16_t period_ms) {
    if (count == 0) count = 1;
    if (count > 10) count = 10;
    if (period_ms < 50)   period_ms = 50;
    if (period_ms > 1000) period_ms = 1000;

    auto* display = getDisplayInstance();
    if (!display) return;

    // Pulse the LEDC duty only; never touch the logical state (s_backlightOn),
    // so a concurrent owner (lock screen / FIDO2 prompt) stays authoritative.
    const TickType_t ticks = pdMS_TO_TICKS(period_ms);
    for (uint8_t i = 0; i < count; ++i) {
        applyBacklight(0);
        vTaskDelay(ticks);
        applyBacklight(s_backlightLevel);
        vTaskDelay(ticks);
    }

    // Re-sync the physical duty to the current logical state.
    cdc::core::MutexGuard guard(s_backlightMutex);
    applyBacklight((s_backlightOn && s_backlightLevel > 0) ? s_backlightLevel : 0);
}

} // namespace cdc::hal
