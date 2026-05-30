#pragma once

#include "cdc_core/IService.h"
#include "esp_err.h"
#include <cstdint>

namespace cdc::hal {

// Opaque device handle
using I2cDeviceHandle = void*;

/**
 * I2C Bus interface
 */
class II2cBus : public core::IService {
public:
    virtual ~II2cBus() = default;

    /**
     * Add a device to the bus
     * @param addr 7-bit I2C address
     * @param out_dev Output device handle
     */
    virtual esp_err_t addDevice(uint8_t addr, I2cDeviceHandle* out_dev) = 0;

    /**
     * Write to a device register
     */
    virtual esp_err_t writeReg(I2cDeviceHandle dev, uint8_t reg,
                               const uint8_t* data, size_t len) = 0;

    /**
     * Read from a device register
     */
    virtual esp_err_t readReg(I2cDeviceHandle dev, uint8_t reg,
                              uint8_t* data, size_t len) = 0;

    /**
     * Raw write transaction to a 7-bit address (no register prefix).
     */
    virtual esp_err_t writeRaw(uint8_t addr, const uint8_t* data, size_t len) = 0;

    /**
     * Raw read transaction from a 7-bit address.
     */
    virtual esp_err_t readRaw(uint8_t addr, uint8_t* data, size_t len) = 0;

    /**
     * Raw write-then-read (repeated start) transaction.
     */
    virtual esp_err_t writeReadRaw(uint8_t addr, const uint8_t* wr, size_t wr_len,
                                   uint8_t* rd, size_t rd_len) = 0;

    /**
     * Probe whether a device ACKs at the given 7-bit address.
     */
    virtual bool probe(uint8_t addr) = 0;

    /**
     * Read from a 16-bit-addressed EEPROM (24Cxx-style) at a byte offset.
     */
    virtual esp_err_t eepromRead(uint8_t addr, uint16_t offset, uint8_t* buf, size_t len) = 0;

    /**
     * Page-aware write to a 16-bit-addressed EEPROM at a byte offset.
     */
    virtual esp_err_t eepromWrite(uint8_t addr, uint16_t offset, const uint8_t* buf, size_t len) = 0;
};

// Factory functions for the two buses
II2cBus* getI2cBus0();  // BQ25895 + TCA9535
II2cBus* getI2cBus1();  // Expansion header

} // namespace cdc::hal
