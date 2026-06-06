/**
 * ESP-IDF SPI port for libtropic (TROPIC01)
 * Uses shared SPI bus with manual chip-select control
 */

#include "cdc_hal/libtropic_port_esp32.h"
#include "cdc_hal/ISpiBus.h"
#include "cdc_hal/hw_config.h"
#include "libtropic_port.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cdc_log.h"
#include <cstdarg>
#include <cstdio>

static const char* TAG = "TR01-SPI";

extern "C" lt_ret_t lt_port_init(lt_l2_state_t *s2) {
    if (!s2 || !s2->device) {
        return LT_PARAM_ERR;
    }

    lt_dev_esp32_t *device = static_cast<lt_dev_esp32_t *>(s2->device);
    device->spi = nullptr;

    // Configure CS pin as output (manual control)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << device->cs_pin;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(device->cs_pin, 1);

    // Initialize shared SPI bus
    if (cdc::hal::initSharedSpiBus() != ESP_OK) {
        return LT_HAL_ERROR;
    }

    // Add TROPIC01 as device on shared SPI bus
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 10 * 1000 * 1000;  // 10 MHz
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;  // Manual CS
    devcfg.queue_size = 1;

    if (spi_bus_add_device(cdc::hal::getSharedSpiHost(), &devcfg, &device->spi) != ESP_OK) {
        return LT_HAL_ERROR;
    }

    LOG_I(TAG, "TROPIC01 SPI initialized (CS=GPIO%d)", device->cs_pin);
    return LT_OK;
}

extern "C" lt_ret_t lt_port_deinit(lt_l2_state_t *s2) {
    if (!s2 || !s2->device) {
        return LT_PARAM_ERR;
    }

    lt_dev_esp32_t *device = static_cast<lt_dev_esp32_t *>(s2->device);
    if (device->spi) {
        spi_bus_remove_device(device->spi);
        device->spi = nullptr;
    }
    gpio_set_level(device->cs_pin, 1);
    return LT_OK;
}

extern "C" lt_ret_t lt_port_spi_csn_low(lt_l2_state_t *s2) {
    if (!s2 || !s2->device) {
        return LT_PARAM_ERR;
    }
    lt_dev_esp32_t *device = static_cast<lt_dev_esp32_t *>(s2->device);
    gpio_set_level(device->cs_pin, 0);
    return LT_OK;
}

extern "C" lt_ret_t lt_port_spi_csn_high(lt_l2_state_t *s2) {
    if (!s2 || !s2->device) {
        return LT_PARAM_ERR;
    }
    lt_dev_esp32_t *device = static_cast<lt_dev_esp32_t *>(s2->device);
    gpio_set_level(device->cs_pin, 1);
    return LT_OK;
}

extern "C" lt_ret_t lt_port_spi_transfer(lt_l2_state_t *s2, uint8_t offset, uint16_t tx_len,
                                         uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!s2 || !s2->device) {
        return LT_PARAM_ERR;
    }
    lt_dev_esp32_t *device = static_cast<lt_dev_esp32_t *>(s2->device);
    if (!device->spi) {
        LOG_E(TAG, "SPI not initialized");
        return LT_HAL_ERROR;
    }

    // In-place SPI transfer (TX/RX share buffer)
    spi_transaction_t t = {};
    t.length = static_cast<size_t>(tx_len) * 8;
    t.tx_buffer = s2->buff + offset;
    t.rx_buffer = s2->buff + offset;
    esp_err_t err = spi_device_polling_transmit(device->spi, &t);

    if (err != ESP_OK) {
        LOG_E(TAG, "SPI transmit failed: %d", err);
        return LT_HAL_ERROR;
    }
    return LT_OK;
}

extern "C" lt_ret_t lt_port_delay(lt_l2_state_t *s2, uint32_t ms) {
    (void)s2;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return LT_OK;
}

#if LT_USE_INT_PIN
extern "C" lt_ret_t lt_port_delay_on_int(lt_l2_state_t *s2, uint32_t ms) {
    (void)s2;
    (void)ms;
    return LT_L1_INT_TIMEOUT;
}
#endif

extern "C" lt_ret_t lt_port_random_bytes(lt_l2_state_t *s2, void *buff, size_t count) {
    (void)s2;
    if (!buff || count == 0) {
        return LT_PARAM_ERR;
    }
    esp_fill_random(buff, count);
    return LT_OK;
}

// Required by libtropic's LT_LOG_* macros (libtropic_logging.h). The format
// strings already carry the level prefix and trailing newline; write them to
// UART0 stdout.
extern "C" int lt_port_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret;
}
