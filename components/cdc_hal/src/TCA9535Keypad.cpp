/**
 * TCA9535 I/O Expander - 12-Key Keypad HAL Implementation
 * Interrupt-driven with FreeRTOS task and circular buffer
 */

#include "cdc_hal/IKeypad.h"
#include "cdc_hal/II2cBus.h"
#include "cdc_hal/hw_config.h"
#include "cdc_core/SystemLock.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "Keypad";

namespace cdc::hal {

/** \brief TCA9535 register-address constants. */
static constexpr uint8_t REG_INPUT_0    = 0x00;
static constexpr uint8_t REG_INPUT_1    = 0x01;
static constexpr uint8_t REG_OUTPUT_0   = 0x02;
static constexpr uint8_t REG_OUTPUT_1   = 0x03;
static constexpr uint8_t REG_POLARITY_0 = 0x04;
static constexpr uint8_t REG_POLARITY_1 = 0x05;
static constexpr uint8_t REG_CONFIG_0   = 0x06;
static constexpr uint8_t REG_CONFIG_1   = 0x07;

/** \brief Keypad task scheduling configuration. */
static constexpr uint32_t TASK_STACK_SIZE = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr uint32_t POLL_TIMEOUT_MS = 50;
static constexpr uint32_t DEBOUNCE_MS = 10;

/** \brief Default long-press detection threshold in milliseconds.
 *  Long enough to avoid accidental triggers, short enough to feel responsive. */
static constexpr uint32_t LONG_PRESS_THRESHOLD_MS = 800;

/** \brief Ring-buffer configuration for queued key events. */
static constexpr size_t KEY_BUFFER_SIZE = 16;

/** \brief Bit positions of each physical key on the TCA9535 P0/P1 ports.
 *  Active-low: a pressed key drives its corresponding bit to 0. */
static constexpr uint8_t KEY_BIT_0   = 0;
static constexpr uint8_t KEY_BIT_1   = 1;
static constexpr uint8_t KEY_BIT_2   = 2;
static constexpr uint8_t KEY_BIT_3   = 3;
static constexpr uint8_t KEY_BIT_4   = 4;
static constexpr uint8_t KEY_BIT_5   = 5;
static constexpr uint8_t KEY_BIT_6   = 6;
static constexpr uint8_t KEY_BIT_7   = 7;
static constexpr uint8_t KEY_BIT_8   = 8;
static constexpr uint8_t KEY_BIT_9   = 9;
static constexpr uint8_t KEY_BIT_NO  = 11;
static constexpr uint8_t KEY_BIT_YES = 10;

/** \brief Mask of all 12 keypad bits (P0.0..P1.3). */
static constexpr uint16_t KEY_MASK_ALL = 0x0FFF;

/** \brief Idle state: every key released, all 12 bits high. */
static constexpr uint16_t KEY_STATE_IDLE = KEY_MASK_ALL;

/** \brief Sentinel value returned when an I2C read fails or no key is mapped. */
static constexpr uint16_t KEY_STATE_INVALID = 0xFFFF;

/** \brief Reserved rescue chord: N and Y held together (anti-block instant lock). */
static constexpr uint16_t PANIC_CHORD_BITS = (1u << KEY_BIT_NO) | (1u << KEY_BIT_YES);

/**
 * \brief Builds the active-low raw state for a single pressed key.
 * \param bit Bit index of the pressed key (0..11).
 * \return 12-bit mask with the selected bit cleared, all others set.
 */
static constexpr uint16_t maskForBit(uint8_t bit) {
    return static_cast<uint16_t>(KEY_MASK_ALL ^ (1u << bit));
}

/**
 * \brief Converts raw 16-bit keypad state to a `Key` enum value.
 * \param raw Raw keypad state bitmask (active-low, bits 0-11).
 * \return Mapped `Key` value, or `KEY_NONE` when no key is active.
 */
static Key rawToKey(uint16_t raw) {
    switch (raw & KEY_MASK_ALL) {
        case maskForBit(KEY_BIT_0):   return Key::KEY_0;
        case maskForBit(KEY_BIT_1):   return Key::KEY_1;
        case maskForBit(KEY_BIT_2):   return Key::KEY_2;
        case maskForBit(KEY_BIT_3):   return Key::KEY_3;
        case maskForBit(KEY_BIT_4):   return Key::KEY_4;
        case maskForBit(KEY_BIT_5):   return Key::KEY_5;
        case maskForBit(KEY_BIT_6):   return Key::KEY_6;
        case maskForBit(KEY_BIT_7):   return Key::KEY_7;
        case maskForBit(KEY_BIT_8):   return Key::KEY_8;
        case maskForBit(KEY_BIT_9):   return Key::KEY_9;
        case maskForBit(KEY_BIT_NO):  return Key::KEY_NO;    // Cancel/N
        case maskForBit(KEY_BIT_YES): return Key::KEY_YES;   // OK/Y
        default: return Key::KEY_NONE;
    }
}

/**
 * \brief Converts a `Key` enum value to its expected raw 12-bit mask.
 * \param key Key to convert.
 * \return Raw keypad mask for the selected key.
 */
static uint16_t keyToMask(Key key) {
    switch (key) {
        case Key::KEY_0:   return maskForBit(KEY_BIT_0);
        case Key::KEY_1:   return maskForBit(KEY_BIT_1);
        case Key::KEY_2:   return maskForBit(KEY_BIT_2);
        case Key::KEY_3:   return maskForBit(KEY_BIT_3);
        case Key::KEY_4:   return maskForBit(KEY_BIT_4);
        case Key::KEY_5:   return maskForBit(KEY_BIT_5);
        case Key::KEY_6:   return maskForBit(KEY_BIT_6);
        case Key::KEY_7:   return maskForBit(KEY_BIT_7);
        case Key::KEY_8:   return maskForBit(KEY_BIT_8);
        case Key::KEY_9:   return maskForBit(KEY_BIT_9);
        case Key::KEY_NO:  return maskForBit(KEY_BIT_NO);
        case Key::KEY_YES: return maskForBit(KEY_BIT_YES);
        default: return KEY_STATE_INVALID;
    }
}

/**
 * Concrete TCA9535 keypad implementation
 */
class TCA9535Keypad : public IKeypad {
public:
    TCA9535Keypad() = default;

    // IService implementation
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "keypad"; }

    // IKeypad implementation
    void poll() override {}  // Handled by task
    bool isKeyPressed(Key key) const override;
    Key getNextKey() override;
    bool hasKey() const override;
    bool anyKeyDown() const override;
    void setCallback(KeyCallback callback) override { callback_ = callback; }
    void setLongPressEnabled(bool enabled, uint32_t thresholdMs) override;
    void setLongPressCallback(LongPressCallback callback) override { longPressCallback_ = callback; }
    void setPanicChordCallback(PanicChordCallback callback) override { panicChordCallback_ = callback; }
    void setDeferShortPress(uint32_t source, bool enabled) override {
        if (enabled) deferSources_ |= source;
        else deferSources_ &= ~source;
    }
    void setKeyRepeat(uint16_t initial_ms, uint16_t period_ms) override {
        keyRepeatInitialMs_ = initial_ms;
        keyRepeatPeriodMs_  = period_ms;
    }
    void prepareForSleep() override;
    void recoverFromSleep() override;
    void clearBuffer() override;

private:
    uint16_t readInputs() const;
    void bufferAddKey(Key key);
    Key bufferGetKey();
    static void taskFunc(void* arg);
    static void IRAM_ATTR isrHandler(void* arg);

    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;

    // I2C device
    II2cBus* bus_ = nullptr;
    I2cDeviceHandle device_ = nullptr;

    // Key buffer (circular) - protected by bufferMux_
    Key keyBuffer_[KEY_BUFFER_SIZE] = {};
    uint8_t bufferHead_ = 0;
    uint8_t bufferTail_ = 0;
    mutable portMUX_TYPE bufferMux_ = portMUX_INITIALIZER_UNLOCKED;

    // Task handles
    SemaphoreHandle_t semaphore_ = nullptr;
    TaskHandle_t taskHandle_ = nullptr;

    // State
    uint16_t lastRawState_ = KEY_STATE_INVALID;
    volatile bool inSleepMode_ = false;

    // Callbacks
    KeyCallback callback_ = nullptr;
    LongPressCallback longPressCallback_ = nullptr;
    PanicChordCallback panicChordCallback_ = nullptr;
    bool panicChordFired_ = false;
    uint32_t deferSources_ = 0;
    bool longPressEnabled_ = false;
    uint32_t longPressThresholdMs_ = LONG_PRESS_THRESHOLD_MS;

    // Long-press tracking
    Key pressedKey_ = Key::KEY_NONE;
    uint32_t pressStartTime_ = 0;
    bool longPressFired_ = false;

    // Key-repeat (re-emit held key; mutually exclusive with long-press)
    uint16_t keyRepeatInitialMs_ = 0;
    uint16_t keyRepeatPeriodMs_  = 0;
    uint32_t lastRepeatMs_       = 0;
};

/**
 * \brief Initializes keypad hardware, ISR, and worker task.
 * \return `true` on successful initialization.
 */
bool TCA9535Keypad::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    // Get I2C bus
    bus_ = getI2cBus0();
    if (!bus_ || bus_->getState() != core::ServiceState::INITIALIZED) {
        LOG_E(TAG, "I2C bus not initialized");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Add TCA9535 device
    if (bus_->addDevice(EXPANDER_ADDR, &device_) != ESP_OK) {
        LOG_E(TAG, "Failed to add TCA9535 device");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Configure TCA9535
    uint8_t allHigh = 0xFF;
    uint8_t noInvert = 0x00;
    uint8_t allInputs = 0xFF;

    // Set output registers high (for proper pull-up reading)
    if (bus_->writeReg(device_, REG_OUTPUT_0, &allHigh, 1) != ESP_OK ||
        bus_->writeReg(device_, REG_OUTPUT_1, &allHigh, 1) != ESP_OK ||
        bus_->writeReg(device_, REG_POLARITY_0, &noInvert, 1) != ESP_OK ||
        bus_->writeReg(device_, REG_POLARITY_1, &noInvert, 1) != ESP_OK ||
        bus_->writeReg(device_, REG_CONFIG_0, &allInputs, 1) != ESP_OK ||
        bus_->writeReg(device_, REG_CONFIG_1, &allInputs, 1) != ESP_OK) {
        LOG_E(TAG, "Failed to configure TCA9535");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Read initial state to clear any pending interrupt
    lastRawState_ = readInputs();
    LOG_I(TAG, "Initial state: 0x%04X", lastRawState_);

    // Create semaphore
    semaphore_ = xSemaphoreCreateBinary();
    if (!semaphore_) {
        LOG_E(TAG, "Failed to create semaphore");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Stack must live in internal RAM, never PSRAM: this task runs the
    // ViewStack dispatch, which performs SPI-flash IO (FAT plugin partition,
    // NVS). The flash driver disables the cache during such operations, which
    // makes a PSRAM-resident stack unreachable (esp_task_stack_is_sane_cache_disabled).
    BaseType_t ret = xTaskCreateWithCaps(taskFunc, "keypad", TASK_STACK_SIZE,
                                  this, TASK_PRIORITY, &taskHandle_,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        LOG_E(TAG, "Failed to create task");
        vSemaphoreDelete(semaphore_);
        semaphore_ = nullptr;
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Configure interrupt pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << EXP_IRQ_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    // Install ISR service (may already be installed by another driver)
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_E(TAG, "Failed to install ISR service: %s", esp_err_to_name(err));
        vSemaphoreDelete(semaphore_);
        semaphore_ = nullptr;
        state_ = core::ServiceState::ERROR;
        return false;
    }
    gpio_isr_handler_add(EXP_IRQ_PIN, isrHandler, this);

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "TCA9535 keypad initialized (IRQ=GPIO%d)", EXP_IRQ_PIN);
    return true;
}

/**
 * \brief Starts keypad service state.
 * \return `true` if service is started after the call.
 */
bool TCA9535Keypad::start() {
    if (state_ == core::ServiceState::INITIALIZED ||
        state_ == core::ServiceState::STOPPED) {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops keypad service state.
 */
void TCA9535Keypad::stop() {
    if (state_ == core::ServiceState::STARTED) {
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Reads raw 16-bit input state from TCA9535.
 * \return Raw input bitmask.
 */
uint16_t TCA9535Keypad::readInputs() const {
    if (!device_) return KEY_STATE_INVALID;

    uint8_t lo = 0xFF, hi = 0xFF;
    if (bus_->readReg(device_, REG_INPUT_0, &lo, 1) != ESP_OK) return KEY_STATE_INVALID;
    if (bus_->readReg(device_, REG_INPUT_1, &hi, 1) != ESP_OK) return KEY_STATE_INVALID;

    return (uint16_t)((hi << 8) | lo);
}

/**
 * \brief Checks whether specified key is currently pressed.
 * \param key Key to test.
 * \return `true` when key is active.
 */
bool TCA9535Keypad::isKeyPressed(Key key) const {
    uint16_t mask = keyToMask(key);
    if (mask == KEY_STATE_INVALID) return false;

    uint16_t current = readInputs();
    return (current & KEY_MASK_ALL) == mask;
}

/**
 * \brief Retrieves next buffered key press.
 * \return Next key or `KEY_NONE`.
 */
Key TCA9535Keypad::getNextKey() {
    return bufferGetKey();
}

/**
 * \brief Returns whether key buffer currently contains entries.
 * \return `true` when a key is available.
 */
bool TCA9535Keypad::hasKey() const {
    portENTER_CRITICAL(&bufferMux_);
    bool hasKey = bufferHead_ != bufferTail_;
    portEXIT_CRITICAL(&bufferMux_);
    return hasKey;
}

/**
 * \brief Returns whether any keypad key is physically held down.
 * \return `true` when any key is pressed.
 */
bool TCA9535Keypad::anyKeyDown() const {
    uint16_t current = readInputs();
    // KEY_STATE_IDLE = all 12 keys released (bits 0-11 high, bits 12-15 don't care)
    return (current & KEY_MASK_ALL) != KEY_STATE_IDLE;
}

/**
 * \brief Enables/disables long-press detection and sets threshold.
 * \param enabled Long-press enable state.
 * \param thresholdMs Threshold in milliseconds.
 */
void TCA9535Keypad::setLongPressEnabled(bool enabled, uint32_t thresholdMs) {
    longPressEnabled_ = enabled;
    longPressThresholdMs_ = thresholdMs;
}

/**
 * \brief Adds key to circular buffer if space is available.
 * \param key Key to enqueue.
 */
void TCA9535Keypad::bufferAddKey(Key key) {
    if (key == Key::KEY_NONE) return;

    portENTER_CRITICAL(&bufferMux_);
    uint8_t nextHead = (bufferHead_ + 1) % KEY_BUFFER_SIZE;
    if (nextHead != bufferTail_) {
        keyBuffer_[bufferHead_] = key;
        bufferHead_ = nextHead;
    }
    portEXIT_CRITICAL(&bufferMux_);
}

/**
 * \brief Pops key from circular buffer.
 * \return Dequeued key or `KEY_NONE`.
 */
Key TCA9535Keypad::bufferGetKey() {
    portENTER_CRITICAL(&bufferMux_);
    if (bufferHead_ == bufferTail_) {
        portEXIT_CRITICAL(&bufferMux_);
        return Key::KEY_NONE;
    }

    Key key = keyBuffer_[bufferTail_];
    bufferTail_ = (bufferTail_ + 1) % KEY_BUFFER_SIZE;
    portEXIT_CRITICAL(&bufferMux_);
    return key;
}

/**
 * \brief GPIO ISR forwarding key interrupt to worker task.
 * \param arg Keypad instance pointer.
 */
void IRAM_ATTR TCA9535Keypad::isrHandler(void* arg) {
    auto* self = static_cast<TCA9535Keypad*>(arg);
    if (self->inSleepMode_) return;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(self->semaphore_, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * \brief Worker task handling debounce, key transitions, and long-press callbacks.
 * \param arg Keypad instance pointer.
 */
void TCA9535Keypad::taskFunc(void* arg) {
    auto* self = static_cast<TCA9535Keypad*>(arg);
    LOG_I(TAG, "Keypad task started");

    while (true) {
        // Wait for IRQ or timeout (poll fallback)
        xSemaphoreTake(self->semaphore_, pdMS_TO_TICKS(POLL_TIMEOUT_MS));

        // Small debounce delay
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        // Drop input events while the system is heading into lockdown:
        // the lockdown window leaves enough time for the e-paper to render
        // the final screen and the user must not be able to mutate any
        // residual state in that window.
        if (cdc::core::SystemLock::instance().isLocked()) {
            continue;
        }

        // Read current state
        uint16_t raw = self->readInputs();

        if (raw != self->lastRawState_) {
            // Count pressed (active-low) bits within the keypad mask.
            // Multiple simultaneous keys are ambiguous: skip both press and release
            // events until the user resolves to a single-key or no-key state.
            uint16_t pressedBits = static_cast<uint16_t>((~raw) & KEY_MASK_ALL);
            uint8_t pressedCount = __builtin_popcount(pressedBits);

            if (pressedCount > 1) {
                // Ambiguous chord-press: emit no single-key events. The reserved
                // N+Y rescue chord is the one recognized combination; fire once
                // per hold.
                if (pressedBits == PANIC_CHORD_BITS && !self->panicChordFired_) {
                    self->panicChordFired_ = true;
                    if (self->panicChordCallback_) self->panicChordCallback_();
                }
            } else {
                self->panicChordFired_ = false;
                Key key = rawToKey(raw);

                // Key press detection
                if (key != Key::KEY_NONE) {
                    // In deferred mode the short press is buffered on release
                    // (and only if no long-press fired), so a hold yields the
                    // long-press alone. Default mode buffers on key-down.
                    if (self->deferSources_ == 0) {
                        self->bufferAddKey(key);
                    }

                    if (self->callback_) {
                        self->callback_(key, true);
                    }

                    // Start long-press / key-repeat tracking
                    if (self->longPressEnabled_) {
                        self->pressedKey_ = key;
                        self->pressStartTime_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        self->lastRepeatMs_ = self->pressStartTime_;
                        self->longPressFired_ = false;
                    }
                } else {
                    // Key release
                    if (self->deferSources_ != 0 && !self->longPressFired_ &&
                        self->pressedKey_ != Key::KEY_NONE) {
                        self->bufferAddKey(self->pressedKey_);
                    }
                    if (self->callback_ && self->pressedKey_ != Key::KEY_NONE) {
                        self->callback_(self->pressedKey_, false);
                    }
                    self->pressedKey_ = Key::KEY_NONE;
                }

                self->lastRawState_ = raw;
            }
        }

        // While a single key is held: either repeat it (key-repeat enabled) or
        // fire the long-press once. The two are mutually exclusive.
        if (self->pressedKey_ != Key::KEY_NONE) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (self->keyRepeatPeriodMs_ > 0) {
                if (now - self->pressStartTime_ >= self->keyRepeatInitialMs_ &&
                    now - self->lastRepeatMs_ >= self->keyRepeatPeriodMs_) {
                    self->lastRepeatMs_ = now;
                    self->bufferAddKey(self->pressedKey_);
                }
            } else if (self->longPressEnabled_ && !self->longPressFired_) {
                if (now - self->pressStartTime_ >= self->longPressThresholdMs_) {
                    self->longPressFired_ = true;
                    if (self->longPressCallback_) {
                        self->longPressCallback_(self->pressedKey_);
                    }
                }
            }
        }
    }
}

/**
 * \brief Prepares keypad interrupt handling for system sleep entry.
 */
void TCA9535Keypad::prepareForSleep() {
    LOG_D(TAG, "Preparing keypad for sleep...");

    // 1. Disable ISR processing flag (prevents ISR from doing anything)
    inSleepMode_ = true;

    // 2. Disable GPIO interrupt at hardware level
    gpio_intr_disable(EXP_IRQ_PIN);
}

/**
 * \brief Restores keypad operation after wakeup.
 */
void TCA9535Keypad::recoverFromSleep() {
    LOG_D(TAG, "Recovering keypad after sleep...");

    // 1. Disable GPIO interrupt (may already be disabled, but be safe)
    gpio_intr_disable(EXP_IRQ_PIN);

    // 2. Wait for all keys to be released (level-triggered wakeup keeps pin LOW)
    int timeout = 200;  // 2 seconds max (200 * 10ms)
    while (anyKeyDown() && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }
    vTaskDelay(pdMS_TO_TICKS(30));  // Extra debounce

    // 3. Clear key buffer (keys from wakeup press)
    clearBuffer();

    // 4. Reset last raw state to current state
    lastRawState_ = readInputs();

    // 5. Re-enable ISR processing flag
    inSleepMode_ = false;

    // 6. Restore edge-triggered interrupt type (wakeup used level-triggered)
    gpio_set_intr_type(EXP_IRQ_PIN, GPIO_INTR_NEGEDGE);

    // 7. Re-enable GPIO interrupt
    gpio_intr_enable(EXP_IRQ_PIN);

    LOG_D(TAG, "Keypad recovered, state: 0x%04X", lastRawState_);
}

/**
 * \brief Clears pending key buffer.
 */
void TCA9535Keypad::clearBuffer() {
    portENTER_CRITICAL(&bufferMux_);
    bufferHead_ = 0;
    bufferTail_ = 0;
    portEXIT_CRITICAL(&bufferMux_);
}

/** \brief Singleton keypad instance. */
static TCA9535Keypad g_keypad;

/**
 * \brief Returns the singleton keypad service instance.
 * \return Pointer to the global `IKeypad` implementation.
 */
IKeypad* getKeypadInstance() {
    return &g_keypad;
}

} // namespace cdc::hal
