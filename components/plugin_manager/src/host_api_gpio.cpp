/**
 * \file host_api_gpio.cpp
 * \brief GPIO / PWM / ADC / I2C / SAO host API with capability + pin-lock.
 *
 * Per-call enforcement on top of the load-time CapabilityChecker:
 *   1. Pin must be allowed by the plugin manifest.
 *   2. Pin must not be locked by another plugin or the native serial CLI.
 *   3. Pin must not be on the firmware-internal block list (Display SPI,
 *      TROPIC01 CS, charger I2C, USB, etc.) - that list is hard-coded.
 *
 * The lock table is also consulted by future GPIO serial commands so a
 * native console session cannot fight a running plugin for the same pin.
 */

#include "cdc_hal/hw_config.h"
#include "cdc_hal/II2cBus.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginGpioPolicy.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#include <array>
#include <cstring>

extern "C" void* plg_get_active_plugin(void);
extern "C" void  plg_log_warn(const char* msg);

namespace {

struct PinLock {
    void* owner = nullptr;
    bool  in_use = false;
};
constexpr size_t MAX_GPIO_PINS = 49;
std::array<PinLock, MAX_GPIO_PINS> s_pin_locks{};

cdc::plugin_manager::Plugin* active() {
    return static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
}

bool manifest_allows_gpio(uint8_t pin) {
    auto* p = active();
    if (!p) return false;
    const auto& cap = p->manifest().capabilities;
    if (cap.grove && (pin == 2 || pin == 3)) return true;
    if (cap.sao   && (pin == 15 || pin == 16)) return true;
    for (uint8_t allowed : cap.gpio_pins) if (allowed == pin) return true;
    for (uint8_t allowed : cap.pwm_pins)  if (allowed == pin) return true;
    for (uint8_t allowed : cap.adc_pins)  if (allowed == pin) return true;
    return false;
}

int acquire_lock(uint8_t pin) {
    if (pin >= MAX_GPIO_PINS) return HOST_ERR_INVALID_ARG;
    if (cdc::plugin_manager::gpio_policy::isBlocked(pin)) return HOST_ERR_NO_CAPABILITY;
    if (!manifest_allows_gpio(pin)) return HOST_ERR_NO_CAPABILITY;

    auto* a = active();
    PinLock& lock = s_pin_locks[pin];
    if (lock.in_use && lock.owner != a) return HOST_ERR_BUSY;
    lock.owner  = a;
    lock.in_use = true;
    return HOST_OK;
}

void release_lock(uint8_t pin) {
    if (pin >= MAX_GPIO_PINS) return;
    s_pin_locks[pin] = PinLock{};
}

// LEDC low-speed channel allocation: each PWM pin gets its own channel so duties
// are independent. All channels share LEDC_TIMER_0 and therefore the frequency
// set by the first host_gpio_pwm_start call.
struct PwmChannel {
    uint8_t pin  = 0;
    bool    used = false;
};
std::array<PwmChannel, LEDC_CHANNEL_MAX> s_pwm_channels{};

int pwm_channel_for(uint8_t pin) {
    for (size_t i = 0; i < s_pwm_channels.size(); ++i)
        if (s_pwm_channels[i].used && s_pwm_channels[i].pin == pin)
            return static_cast<int>(i);
    return -1;
}

int pwm_channel_alloc(uint8_t pin) {
    for (size_t i = 0; i < s_pwm_channels.size(); ++i) {
        if (!s_pwm_channels[i].used) {
            s_pwm_channels[i] = { pin, true };
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Only the expansion bus (I2C1) is reachable by plugins. Bus 0 carries the
// charger (BQ25895) and IO expander (TCA9535) and is never exposed. The
// TROPIC01 is on SPI, not I2C, so it is unreachable here by design.
bool manifest_allows_i2c(uint8_t bus) {
    auto* p = active();
    if (!p) return false;
    for (uint8_t b : p->manifest().capabilities.i2c_bus) if (b == bus) return true;
    return false;
}

// Fetch the expansion bus, ensuring its driver is installed (idempotent).
cdc::hal::II2cBus* expansion_bus() {
    auto* bus = cdc::hal::getI2cBus1();
    return (bus && bus->init()) ? bus : nullptr;
}

}  // namespace

extern "C" {

int host_gpio_set_direction(uint8_t pin, uint8_t direction)
{
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;

    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    switch (direction) {
        case GPIO_DIR_IN:     cfg.mode = GPIO_MODE_INPUT; break;
        case GPIO_DIR_OUT:    cfg.mode = GPIO_MODE_OUTPUT; break;
        case GPIO_DIR_OUT_OD: cfg.mode = GPIO_MODE_OUTPUT_OD; break;
        default:              return HOST_ERR_INVALID_ARG;
    }
    return gpio_config(&cfg) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_gpio_set_pull(uint8_t pin, uint8_t pull)
{
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;

    gpio_pull_mode_t mode = GPIO_FLOATING;
    switch (pull) {
        case GPIO_PULL_UP:   mode = GPIO_PULLUP_ONLY; break;
        case GPIO_PULL_DOWN: mode = GPIO_PULLDOWN_ONLY; break;
        case GPIO_PULL_NONE: mode = GPIO_FLOATING; break;
        default:             return HOST_ERR_INVALID_ARG;
    }
    return gpio_set_pull_mode(static_cast<gpio_num_t>(pin), mode) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_gpio_write(uint8_t pin, bool level)
{
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;
    return gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_gpio_read(uint8_t pin, bool* level)
{
    if (!level) return HOST_ERR_INVALID_ARG;
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;
    *level = gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
    return HOST_OK;
}

int host_gpio_release(uint8_t pin)
{
    if (pin >= MAX_GPIO_PINS) return HOST_ERR_INVALID_ARG;
    // Only the owner can release; silently ignore alien plugins.
    if (s_pin_locks[pin].owner != active()) return HOST_OK;
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
    release_lock(pin);
    return HOST_OK;
}

int host_gpio_pwm_start(uint8_t pin, uint32_t freq_hz, uint16_t duty_per_mille)
{
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;
    if (duty_per_mille > 1000) return HOST_ERR_INVALID_ARG;

    int ch = pwm_channel_for(pin);
    const bool fresh = (ch < 0);
    if (fresh) ch = pwm_channel_alloc(pin);
    if (ch < 0) return HOST_ERR_NO_MEMORY;

    static bool s_timer_inited = false;
    if (!s_timer_inited) {
        ledc_timer_config_t timer{};
        timer.speed_mode      = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num       = LEDC_TIMER_0;
        timer.freq_hz         = freq_hz ? freq_hz : 5000;
        timer.clk_cfg         = LEDC_AUTO_CLK;
        if (ledc_timer_config(&timer) != ESP_OK) {
            if (fresh) s_pwm_channels[ch] = PwmChannel{};
            return HOST_ERR_GENERIC;
        }
        s_timer_inited = true;
    }

    ledc_channel_config_t cfg{};
    cfg.channel    = static_cast<ledc_channel_t>(ch);
    cfg.duty       = (1023 * duty_per_mille) / 1000;
    cfg.gpio_num   = pin;
    cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    cfg.hpoint     = 0;
    cfg.timer_sel  = LEDC_TIMER_0;
    if (ledc_channel_config(&cfg) != ESP_OK) {
        if (fresh) s_pwm_channels[ch] = PwmChannel{};
        return HOST_ERR_GENERIC;
    }
    return HOST_OK;
}

int host_gpio_pwm_set_duty(uint8_t pin, uint16_t duty_per_mille)
{
    if (duty_per_mille > 1000) return HOST_ERR_INVALID_ARG;
    int ch = pwm_channel_for(pin);
    if (ch < 0) return HOST_ERR_INVALID_ARG;
    auto channel  = static_cast<ledc_channel_t>(ch);
    uint32_t duty = (1023 * duty_per_mille) / 1000;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty) != ESP_OK) return HOST_ERR_GENERIC;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) != ESP_OK)    return HOST_ERR_GENERIC;
    return HOST_OK;
}

int host_gpio_pwm_stop(uint8_t pin)
{
    int ch = pwm_channel_for(pin);
    if (ch < 0) return HOST_ERR_INVALID_ARG;
    ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(ch), 0);
    s_pwm_channels[ch] = PwmChannel{};
    return HOST_OK;
}

int host_adc_read(uint8_t pin, uint16_t* raw, uint16_t* millivolt)
{
    if (!raw && !millivolt) return HOST_ERR_INVALID_ARG;
    int rc = acquire_lock(pin);
    if (rc != HOST_OK) return rc;

    // ADC1 channel mapping for user pins (ADC2 conflicts with WiFi).
    struct AdcMap { uint8_t pin; adc_channel_t ch; };
    constexpr AdcMap PINS[] = {
        { 2, ADC_CHANNEL_1 }, { 3, ADC_CHANNEL_2 },
        { 4, ADC_CHANNEL_3 }, { 5, ADC_CHANNEL_4 },
        { 6, ADC_CHANNEL_5 }, { 7, ADC_CHANNEL_6 },
        { 9, ADC_CHANNEL_8 },
    };
    adc_channel_t ch = static_cast<adc_channel_t>(-1);
    for (const auto& m : PINS) if (m.pin == pin) ch = m.ch;
    if (static_cast<int>(ch) < 0) return HOST_ERR_NOT_SUPPORTED;

    adc_oneshot_unit_handle_t unit;
    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id  = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit_cfg, &unit) != ESP_OK) return HOST_ERR_GENERIC;

    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit, ch, &chan_cfg) != ESP_OK) {
        adc_oneshot_del_unit(unit);
        return HOST_ERR_GENERIC;
    }

    int reading = 0;
    esp_err_t err = adc_oneshot_read(unit, ch, &reading);
    adc_oneshot_del_unit(unit);
    if (err != ESP_OK) return HOST_ERR_GENERIC;

    if (raw) *raw = static_cast<uint16_t>(reading);
    if (millivolt) {
        // Rough conversion without calibration; ATTEN_DB_12 gives ~3300 mV full-scale on 12-bit.
        *millivolt = static_cast<uint16_t>(reading * 3300 / 4095);
    }
    return HOST_OK;
}

// I2C and SAO EEPROM are thin capability gates over the I2cBus HAL; the raw
// transaction logic lives in cdc_hal/I2cBus, not here.

int host_i2c_write(uint8_t bus, uint8_t addr, const uint8_t* data, size_t len)
{
    if (!data && len) return HOST_ERR_INVALID_ARG;
    if (bus != 1) return HOST_ERR_INVALID_ARG;            // only the expansion bus
    if (!manifest_allows_i2c(bus)) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;
    return b->writeRaw(addr, data, len) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_i2c_read(uint8_t bus, uint8_t addr, uint8_t* data, size_t len)
{
    if (!data || len == 0) return HOST_ERR_INVALID_ARG;
    if (bus != 1) return HOST_ERR_INVALID_ARG;
    if (!manifest_allows_i2c(bus)) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;
    return b->readRaw(addr, data, len) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_i2c_write_read(uint8_t bus, uint8_t addr,
                        const uint8_t* wr, size_t wr_len,
                        uint8_t* rd, size_t rd_len)
{
    if ((!wr && wr_len) || !rd || rd_len == 0) return HOST_ERR_INVALID_ARG;
    if (bus != 1) return HOST_ERR_INVALID_ARG;
    if (!manifest_allows_i2c(bus)) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;
    return b->writeReadRaw(addr, wr, wr_len, rd, rd_len) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_i2c_scan(uint8_t bus, uint8_t* found_addrs, size_t* count)
{
    if (!found_addrs || !count) return HOST_ERR_INVALID_ARG;
    if (bus != 1) return HOST_ERR_INVALID_ARG;
    if (!manifest_allows_i2c(bus)) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;

    const size_t cap = *count;
    size_t n = 0;
    for (uint8_t addr = 0x08; addr < 0x78 && n < cap; ++addr) {
        if (b->probe(addr)) found_addrs[n++] = addr;
    }
    *count = n;
    return HOST_OK;
}

int host_sao_eeprom_read(uint16_t off, uint8_t* buf, size_t len)
{
    if (!buf || len == 0) return HOST_ERR_INVALID_ARG;
    auto* p = active();
    if (!p || !p->manifest().capabilities.sao) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;
    return b->eepromRead(SAO_EEPROM_ADDR, off, buf, len) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_sao_eeprom_write(uint16_t off, const uint8_t* buf, size_t len)
{
    if (!buf || len == 0) return HOST_ERR_INVALID_ARG;
    auto* p = active();
    if (!p || !p->manifest().capabilities.sao) return HOST_ERR_NO_CAPABILITY;
    auto* b = expansion_bus();
    if (!b) return HOST_ERR_GENERIC;
    return b->eepromWrite(SAO_EEPROM_ADDR, off, buf, len) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

}  // extern "C"
