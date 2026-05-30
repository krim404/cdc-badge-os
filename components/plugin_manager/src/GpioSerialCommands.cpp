/**
 * \file GpioSerialCommands.cpp
 * \brief Native serial mini-module for direct GPIO / ADC / I2C poking.
 *
 * Same pin whitelist + hard block list as the plugin GPIO host API. No
 * capability check (the user is physically holding the badge), but firmware-
 * internal pins (TROPIC01 SPI, display SPI, charger, USB, PSRAM) are still
 * blocked at the static block list level.
 *
 * Subcommands:
 *   GPIO LIST                                  pins on the whitelist + current mode
 *   GPIO MODE <pin> in|out|out_od [pull_up|pull_down|none]
 *   GPIO WRITE <pin> 0|1
 *   GPIO READ  <pin>
 *   GPIO RELEASE <pin>
 *   ADC READ <pin>                             oneshot ADC (mV)
 *   I2C SCAN <bus>                             list 7-bit addrs that ACK
 */

#include "cdc_log.h"
#include "plugin_manager/PluginGpioPolicy.h"
#include "serial_cmd/ICommandRegistry.h"
#include "HexUtil.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cdc::plugin_manager {

static const char* TAG = "GPIO_CMD";
static const char* CMD_MODULE = "gpio";

namespace {

inline bool is_allowed(uint8_t pin) { return gpio_policy::isAllowed(pin); }

void send(const char* line) { std::printf("%s\n", line); }
void sendf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}

void cmd_list()
{
    send("pin  status");
    for (size_t i = 0; i < gpio_policy::ALLOWED_COUNT; ++i) {
        sendf(" %2u  free", static_cast<unsigned>(gpio_policy::ALLOWED[i]));
    }
}

void cmd_mode(const char* args)
{
    char dir_buf[16] = {0};
    char pull_buf[16] = "none";
    unsigned long pin_l = 0;
    int n = std::sscanf(args, "%lu %15s %15s", &pin_l, dir_buf, pull_buf);
    if (n < 2) { send("ERR usage_pin_mode_pull"); return; }
    uint8_t pin = static_cast<uint8_t>(pin_l);
    if (!is_allowed(pin)) { send("ERR pin_not_allowed"); return; }

    gpio_mode_t mode;
    if      (std::strcmp(dir_buf, "in")     == 0) mode = GPIO_MODE_INPUT;
    else if (std::strcmp(dir_buf, "out")    == 0) mode = GPIO_MODE_OUTPUT;
    else if (std::strcmp(dir_buf, "out_od") == 0) mode = GPIO_MODE_OUTPUT_OD;
    else { send("ERR mode_in_out_out_od"); return; }

    gpio_pull_mode_t pull = GPIO_FLOATING;
    if      (std::strcmp(pull_buf, "pull_up")   == 0) pull = GPIO_PULLUP_ONLY;
    else if (std::strcmp(pull_buf, "pull_down") == 0) pull = GPIO_PULLDOWN_ONLY;

    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.mode         = mode;
    cfg.pull_up_en   = pull == GPIO_PULLUP_ONLY   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = pull == GPIO_PULLDOWN_ONLY ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) { send("ERR gpio_config"); return; }
    send("OK");
}

void cmd_write(const char* args)
{
    unsigned long pin_l = 0;
    int level = 0;
    if (std::sscanf(args, "%lu %d", &pin_l, &level) != 2) { send("ERR usage_pin_level"); return; }
    uint8_t pin = static_cast<uint8_t>(pin_l);
    if (!is_allowed(pin)) { send("ERR pin_not_allowed"); return; }
    if (gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0) != ESP_OK) {
        send("ERR set_level"); return;
    }
    send("OK");
}

void cmd_read(const char* args)
{
    unsigned long pin_l = 0;
    if (std::sscanf(args, "%lu", &pin_l) != 1) { send("ERR usage_pin"); return; }
    uint8_t pin = static_cast<uint8_t>(pin_l);
    if (!is_allowed(pin)) { send("ERR pin_not_allowed"); return; }
    sendf("%d", gpio_get_level(static_cast<gpio_num_t>(pin)));
}

void cmd_release(const char* args)
{
    unsigned long pin_l = 0;
    if (std::sscanf(args, "%lu", &pin_l) != 1) { send("ERR usage_pin"); return; }
    uint8_t pin = static_cast<uint8_t>(pin_l);
    if (!is_allowed(pin)) { send("ERR pin_not_allowed"); return; }
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
    send("OK");
}

void gpioDispatch(const char* args)
{
    if (!args || !*args) { send("ERR missing_subcmd"); return; }
    while (*args == ' ') args++;
    const char* space = std::strchr(args, ' ');
    size_t sub_len = space ? static_cast<size_t>(space - args) : std::strlen(args);
    const char* rest = space ? space + 1 : "";

    auto eq = [&](const char* w) {
        return std::strlen(w) == sub_len && strncasecmp(args, w, sub_len) == 0;
    };

    if      (eq("LIST"))    cmd_list();
    else if (eq("MODE"))    cmd_mode(rest);
    else if (eq("WRITE"))   cmd_write(rest);
    else if (eq("READ"))    cmd_read(rest);
    else if (eq("RELEASE")) cmd_release(rest);
    else                    send("ERR unknown_subcmd");
}

// ESP32-S3 ADC1 channel mapping for the user-accessible pins.
// (ADC2 is unreliable when WiFi is active, so we expose only ADC1 here.)
struct AdcMap { uint8_t pin; adc_channel_t ch; };
constexpr AdcMap ADC1_PINS[] = {
    { 2, ADC_CHANNEL_1 }, { 3, ADC_CHANNEL_2 },
    { 4, ADC_CHANNEL_3 }, { 5, ADC_CHANNEL_4 },
    { 6, ADC_CHANNEL_5 }, { 7, ADC_CHANNEL_6 },
    { 9, ADC_CHANNEL_8 },
};

bool pin_to_adc(uint8_t pin, adc_channel_t& ch_out) {
    for (const auto& m : ADC1_PINS) {
        if (m.pin == pin) { ch_out = m.ch; return true; }
    }
    return false;
}

void adcDispatch(const char* args)
{
    if (!args || strncasecmp(args, "READ", 4) != 0) { send("ERR usage_ADC_READ_pin"); return; }
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    unsigned long pin_l = 0;
    if (std::sscanf(args, "%lu", &pin_l) != 1) { send("ERR usage_pin"); return; }
    uint8_t pin = static_cast<uint8_t>(pin_l);

    adc_channel_t ch;
    if (!pin_to_adc(pin, ch)) { send("ERR pin_no_adc1"); return; }

    adc_oneshot_unit_handle_t unit;
    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id  = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit_cfg, &unit) != ESP_OK) { send("ERR adc_unit"); return; }

    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit, ch, &chan_cfg) != ESP_OK) {
        send("ERR adc_chan_config");
        adc_oneshot_del_unit(unit);
        return;
    }
    int raw = 0;
    if (adc_oneshot_read(unit, ch, &raw) != ESP_OK) { send("ERR adc_read"); }
    else sendf("raw=%d", raw);
    adc_oneshot_del_unit(unit);
}

void i2cDispatch(const char* args)
{
    if (!args || strncasecmp(args, "SCAN", 4) != 0) { send("ERR usage_I2C_SCAN_bus"); return; }
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    unsigned long bus_l = 1;
    std::sscanf(args, "%lu", &bus_l);
    if (bus_l == 0) { send("ERR bus0_reserved"); return; }

    i2c_port_t port = static_cast<i2c_port_t>(bus_l);
    bool any = false;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            sendf("0x%02X", addr);
            any = true;
        }
    }
    if (!any) send("(none)");
}

// SAO EEPROM (24CXX-style, I2C1 @ 0x50, 16-bit register address) ------------

constexpr uint8_t    SAO_EEPROM_ADDR = 0x50;
constexpr i2c_port_t SAO_I2C_PORT    = static_cast<i2c_port_t>(1);

void sao_read(const char* args)
{
    unsigned long offset = 0, count = 0;
    if (std::sscanf(args, "%lu %lu", &offset, &count) != 2) {
        send("ERR usage_offset_count"); return;
    }
    if (count == 0 || count > 256) { send("ERR count_1_256"); return; }

    uint8_t reg[2] = { static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset & 0xff) };
    uint8_t buf[256] = {0};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SAO_EEPROM_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, reg, sizeof(reg), true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SAO_EEPROM_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, count, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SAO_I2C_PORT, cmd, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) { send("ERR i2c"); return; }

    char hex[513];
    for (size_t i = 0; i < count; ++i) std::snprintf(hex + i * 2, 3, "%02X", buf[i]);
    hex[count * 2] = '\0';
    send(hex);
}

void sao_write(const char* args)
{
    unsigned long offset = 0;
    int consumed = 0;
    if (std::sscanf(args, "%lu %n", &offset, &consumed) != 1 || consumed == 0) {
        send("ERR usage_offset_hex"); return;
    }
    const char* hex = args + consumed;
    size_t hex_len = std::strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) { send("ERR hex_len"); return; }
    size_t bytes = hex_len / 2;
    if (bytes > 128) { send("ERR max_128"); return; }

    uint8_t data[128];
    for (size_t i = 0; i < bytes; ++i) {
        int h = hex_val(hex[i * 2]), l = hex_val(hex[i * 2 + 1]);
        if (h < 0 || l < 0) { send("ERR hex"); return; }
        data[i] = static_cast<uint8_t>((h << 4) | l);
    }

    uint8_t reg[2] = { static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset & 0xff) };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SAO_EEPROM_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, reg, sizeof(reg), true);
    i2c_master_write(cmd, data, bytes, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SAO_I2C_PORT, cmd, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) { send("ERR i2c"); return; }
    send("OK");
}

void saoDispatch(const char* args)
{
    if (!args || !*args) { send("ERR missing_subcmd"); return; }
    while (*args == ' ') args++;
    const char* space = std::strchr(args, ' ');
    size_t sub_len = space ? static_cast<size_t>(space - args) : std::strlen(args);
    const char* rest = space ? space + 1 : "";

    auto eq = [&](const char* w) {
        return std::strlen(w) == sub_len && strncasecmp(args, w, sub_len) == 0;
    };

    if      (eq("EEPROM_READ"))  sao_read(rest);
    else if (eq("EEPROM_WRITE")) sao_write(rest);
    else                          send("ERR usage_SAO_EEPROM_READ_WRITE");
}

}  // namespace

void registerGpioSerialCommands()
{
    auto& reg = cdc::serial::getCommandRegistry();
    reg.registerCommand({"GPIO", "GPIO LIST/MODE/WRITE/READ/RELEASE on user pins", gpioDispatch, CMD_MODULE, true});
    reg.registerCommand({"ADC",  "ADC READ <pin>",                                  adcDispatch,  CMD_MODULE, true});
    reg.registerCommand({"I2C",  "I2C SCAN <bus>",                                  i2cDispatch,  CMD_MODULE, true});
    reg.registerCommand({"SAO",  "SAO EEPROM_READ <off> <count> | EEPROM_WRITE <off> <hex>", saoDispatch, CMD_MODULE, true});
    LOG_I(TAG, "GPIO/ADC/I2C/SAO serial commands registered");
}

}  // namespace cdc::plugin_manager
