#pragma once

#include <cstdint>

namespace cdc::mod_otphid {

/** \brief Yubico OTP HID feature/payload frame size (ykdef.h FEATURE_RPT_SIZE). */
static constexpr uint8_t FEATURE_RPT_SIZE = 8;

} // namespace cdc::mod_otphid
