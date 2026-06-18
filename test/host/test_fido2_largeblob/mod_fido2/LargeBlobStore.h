/**
 * \file LargeBlobStore.h (shim)
 * \brief Include-path shim resolving the firmware LargeBlobStore.h on host.
 *
 * The native test builder adds this test folder to the include path, so the
 * quote-include in LargeBlobStore.cpp resolves to the real firmware header.
 */
#pragma once
#include "../../../../components/mod_fido2/include/mod_fido2/LargeBlobStore.h"
