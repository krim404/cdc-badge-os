/**
 * \file MessageTypes.h (shim)
 * \brief Include-path shim resolving the firmware MessageTypes.h on host.
 *
 * MessageTypes.h is self-contained (only <cstdint>), so the test pins its real
 * constants. The native test builder adds this test folder to the include
 * path, so the quote-include below resolves to the real firmware header.
 */
#pragma once
#include "../../../../components/cdc_msg/include/cdc_msg/MessageTypes.h"
