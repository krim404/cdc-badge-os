/**
 * \file Cp437.h (shim)
 * \brief Include-path shim so the firmware Cp437.cpp can be compiled on host.
 *
 * The native PlatformIO env adds no component include dirs. Cp437.cpp does
 * `#include "cdc_core/Cp437.h"`; the native test builder puts this test folder
 * on the include path, so this file resolves that quote-include to the real
 * firmware header by a project-relative path. This keeps test_cp437.cpp a
 * direct unit test of the firmware codec source, not a reimplementation.
 */
#pragma once
#include "../../../../components/cdc_core/include/cdc_core/Cp437.h"
