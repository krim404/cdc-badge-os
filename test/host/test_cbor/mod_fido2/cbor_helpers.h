/**
 * \file cbor_helpers.h (shim)
 * \brief Include-path shim resolving the firmware cbor_helpers.h on host.
 *
 * The native test builder adds this test folder to the include path, so the
 * quote-include in cbor_helpers.cpp resolves to the real firmware header.
 */
#pragma once
#include "../../../../components/mod_fido2/include/mod_fido2/cbor_helpers.h"
