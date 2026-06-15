#pragma once

#include <cstdint>

/**
 * \file MessageProfile.h
 * \brief 128-bit GATT UUIDs for the message-transfer service.
 *
 * Fresh random base UUID (canonical CDC5000x-B0A7-4E3D-9C82-5A6F1B3E9D2C),
 * byte-array form is little-endian for hal::BleUuid::from128. The discriminator
 * lives at index 12 (0x01 service, 0x02 control, 0x03 status, 0x04 data),
 * mirroring the Nordic UART Service layout.
 */

namespace cdc::msg {

/// Service UUID (CDC50001-...): advertised so scanners can find a badge.
inline constexpr uint8_t kMsgServiceUuid[16] = {
    0x2C, 0x9D, 0x3E, 0x1B, 0x6F, 0x5A, 0x82, 0x9C,
    0x3D, 0x4E, 0xA7, 0xB0, 0x01, 0x00, 0xC5, 0xCD,
};

/// Control characteristic (CDC50002-...): plaintext WRITE, sender -> receiver.
inline constexpr uint8_t kMsgControlUuid[16] = {
    0x2C, 0x9D, 0x3E, 0x1B, 0x6F, 0x5A, 0x82, 0x9C,
    0x3D, 0x4E, 0xA7, 0xB0, 0x02, 0x00, 0xC5, 0xCD,
};

/// Status characteristic (CDC50003-...): plaintext NOTIFY, receiver -> sender.
inline constexpr uint8_t kMsgStatusUuid[16] = {
    0x2C, 0x9D, 0x3E, 0x1B, 0x6F, 0x5A, 0x82, 0x9C,
    0x3D, 0x4E, 0xA7, 0xB0, 0x03, 0x00, 0xC5, 0xCD,
};

/// Data characteristic (CDC50004-...): encrypted WRITE, sender -> receiver.
inline constexpr uint8_t kMsgDataUuid[16] = {
    0x2C, 0x9D, 0x3E, 0x1B, 0x6F, 0x5A, 0x82, 0x9C,
    0x3D, 0x4E, 0xA7, 0xB0, 0x04, 0x00, 0xC5, 0xCD,
};

}  // namespace cdc::msg
