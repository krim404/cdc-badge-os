/**
 * I2C Bus Implementation
 * Uses ESP-IDF legacy I2C driver for stability
 */

#include "cdc_hal/II2cBus.h"
#include "cdc_hal/hw_config.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cdc_log.h"

static const char* TAG = "I2cBus";

namespace cdc::hal {

/** \brief I2C bus timing configuration constants. */
static constexpr uint32_t I2C_FREQ_HZ = 100000;  // 100kHz standard mode
static constexpr uint32_t I2C_TIMEOUT_MS = 100;

/**
 * Internal device structure
 */
struct I2cDevice {
    i2c_port_t port;
    uint8_t addr;
};

/**
 * Concrete I2C bus implementation using legacy driver
 */
class I2cBusImpl : public II2cBus {
public:
    I2cBusImpl(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, const char* name)
        : port_(port), sda_(sda), scl_(scl), name_(name) {}

    // IService implementation
    bool init() override;
    bool start() override { return state_ == core::ServiceState::INITIALIZED; }
    void stop() override {}
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return name_; }

    // II2cBus implementation
    esp_err_t addDevice(uint8_t addr, I2cDeviceHandle* out_dev) override;
    esp_err_t writeReg(I2cDeviceHandle dev, uint8_t reg,
                       const uint8_t* data, size_t len) override;
    esp_err_t readReg(I2cDeviceHandle dev, uint8_t reg,
                      uint8_t* data, size_t len) override;
    esp_err_t writeRaw(uint8_t addr, const uint8_t* data, size_t len) override;
    esp_err_t readRaw(uint8_t addr, uint8_t* data, size_t len) override;
    esp_err_t writeReadRaw(uint8_t addr, const uint8_t* wr, size_t wr_len,
                           uint8_t* rd, size_t rd_len) override;
    bool probe(uint8_t addr) override;
    esp_err_t eepromRead(uint8_t addr, uint16_t offset, uint8_t* buf, size_t len) override;
    esp_err_t eepromWrite(uint8_t addr, uint16_t offset, const uint8_t* buf, size_t len) override;

private:
    i2c_port_t port_;
    gpio_num_t sda_;
    gpio_num_t scl_;
    const char* name_;
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;

    // Static device pool (avoid heap allocation)
    static constexpr size_t MAX_DEVICES = 4;
    I2cDevice devices_[MAX_DEVICES] = {};
    size_t deviceCount_ = 0;
};

/**
 * \brief Initializes hardware I2C controller and driver.
 * \return `true` if initialization succeeded.
 */
bool I2cBusImpl::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED;
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda_;
    conf.scl_io_num = scl_;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_FREQ_HZ;
    conf.clk_flags = 0;

    esp_err_t err = i2c_param_config(port_, &conf);
    if (err != ESP_OK) {
        LOG_E(TAG, "%s: i2c_param_config failed: %d", name_, err);
        state_ = core::ServiceState::ERROR;
        return false;
    }

    err = i2c_driver_install(port_, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        LOG_E(TAG, "%s: i2c_driver_install failed: %d", name_, err);
        state_ = core::ServiceState::ERROR;
        return false;
    }

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "%s initialized (SDA=%d, SCL=%d)", name_, sda_, scl_);
    return true;
}

/**
 * \brief Registers an I2C target device in static device pool.
 * \param addr 7-bit device address.
 * \param out_dev Output handle for registered device.
 * \return ESP-IDF status code.
 */
esp_err_t I2cBusImpl::addDevice(uint8_t addr, I2cDeviceHandle* out_dev) {
    if (deviceCount_ >= MAX_DEVICES) {
        LOG_E(TAG, "%s: device pool full", name_);
        return ESP_ERR_NO_MEM;
    }

    I2cDevice* dev = &devices_[deviceCount_++];
    dev->port = port_;
    dev->addr = addr;
    *out_dev = dev;

    LOG_I(TAG, "%s: added device at 0x%02X", name_, addr);
    return ESP_OK;
}

/**
 * \brief Writes bytes to a register of an I2C device.
 * \param handle Device handle.
 * \param reg Register address.
 * \param data Data buffer.
 * \param len Number of bytes to write.
 * \return ESP-IDF status code.
 */
esp_err_t I2cBusImpl::writeReg(I2cDeviceHandle handle, uint8_t reg,
                               const uint8_t* data, size_t len) {
    auto* dev = static_cast<I2cDevice*>(handle);
    if (!dev) return ESP_ERR_INVALID_ARG;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (data && len > 0) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(dev->port, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    return err;
}

/**
 * \brief Reads bytes from a register of an I2C device.
 * \param handle Device handle.
 * \param reg Register address.
 * \param data Output data buffer.
 * \param len Number of bytes to read.
 * \return ESP-IDF status code.
 */
esp_err_t I2cBusImpl::readReg(I2cDeviceHandle handle, uint8_t reg,
                              uint8_t* data, size_t len) {
    auto* dev = static_cast<I2cDevice*>(handle);
    if (!dev || !data || len == 0) return ESP_ERR_INVALID_ARG;

    // Write register address
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);

    // Repeated start and read
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(dev->port, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    return err;
}

esp_err_t I2cBusImpl::writeRaw(uint8_t addr, const uint8_t* data, size_t len) {
    if (!data && len > 0) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (len > 0) i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port_, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t I2cBusImpl::readRaw(uint8_t addr, uint8_t* data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port_, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t I2cBusImpl::writeReadRaw(uint8_t addr, const uint8_t* wr, size_t wr_len,
                                   uint8_t* rd, size_t rd_len) {
    if ((!wr && wr_len > 0) || !rd || rd_len == 0) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (wr_len > 0) i2c_master_write(cmd, wr, wr_len, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, rd, rd_len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port_, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

bool I2cBusImpl::probe(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port_, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

esp_err_t I2cBusImpl::eepromRead(uint8_t addr, uint16_t offset, uint8_t* buf, size_t len) {
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    uint8_t reg[2] = { static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset & 0xff) };
    return writeReadRaw(addr, reg, sizeof(reg), buf, len);
}

esp_err_t I2cBusImpl::eepromWrite(uint8_t addr, uint16_t offset, const uint8_t* buf, size_t len) {
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    constexpr size_t PAGE = 16;  // 24Cxx page; a write must not cross a page boundary
    size_t written = 0;
    while (written < len) {
        uint16_t cur = static_cast<uint16_t>(offset + written);
        size_t room = PAGE - (cur % PAGE);
        size_t chunk = (len - written < room) ? (len - written) : room;
        uint8_t frame[2 + PAGE];
        frame[0] = static_cast<uint8_t>(cur >> 8);
        frame[1] = static_cast<uint8_t>(cur & 0xff);
        for (size_t i = 0; i < chunk; ++i) frame[2 + i] = buf[written + i];
        esp_err_t err = writeRaw(addr, frame, 2 + chunk);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(5));  // EEPROM self-timed write cycle
        written += chunk;
    }
    return ESP_OK;
}

/** \brief Singleton instances for both hardware I2C ports. */
static I2cBusImpl g_i2c0(I2C_NUM_0, I2C0_SDA_PIN, I2C0_SCL_PIN, "i2c0");
static I2cBusImpl g_i2c1(I2C_NUM_1, I2C1_SDA_PIN, I2C1_SCL_PIN, "i2c1");

/**
 * \brief Returns singleton instance of I2C bus 0.
 * \return Pointer to `II2cBus` instance.
 */
II2cBus* getI2cBus0() { return &g_i2c0; }
/**
 * \brief Returns singleton instance of I2C bus 1.
 * \return Pointer to `II2cBus` instance.
 */
II2cBus* getI2cBus1() { return &g_i2c1; }

} // namespace cdc::hal
