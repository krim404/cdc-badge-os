#pragma once

/**
 * \file
 * \brief Build-time selection of the TROPIC01 SH0 pairing key and slot.
 *
 * Every R-Config / maintenance-mode operation needs a secure channel session,
 * which is only possible with a known pairing key. A chip provisioned with a
 * non-default key cannot be reached with the default production key, so the key
 * is selectable at build time.
 *
 * Select via platformio.ini build_flags (default: PROD0):
 *   -DCDC_PAIRING_KEY=CDC_PAIRING_KEY_PROD0       libtropic production key, slot 0
 *   -DCDC_PAIRING_KEY=CDC_PAIRING_KEY_ENG_SAMPLE  libtropic engineering-sample key, slot 0
 *   -DCDC_PAIRING_KEY=CDC_PAIRING_KEY_CUSTOM      key from pairing_key_custom.h (git-ignored)
 *
 * The selection resolves to three macros used by the secure-element layer:
 *   PAIRING_KEY_PRIV, PAIRING_KEY_PUB, PAIRING_KEY_SLOT.
 */

#include <stdint.h>

#include "libtropic_common.h"

#define CDC_PAIRING_KEY_PROD0      0
#define CDC_PAIRING_KEY_ENG_SAMPLE 1
#define CDC_PAIRING_KEY_CUSTOM     2

#ifndef CDC_PAIRING_KEY
#define CDC_PAIRING_KEY CDC_PAIRING_KEY_PROD0
#endif

#if CDC_PAIRING_KEY == CDC_PAIRING_KEY_PROD0
#define PAIRING_KEY_PRIV lt_sh0priv_prod0
#define PAIRING_KEY_PUB  lt_sh0pub_prod0
#define PAIRING_KEY_SLOT TR01_PAIRING_KEY_SLOT_INDEX_0

#elif CDC_PAIRING_KEY == CDC_PAIRING_KEY_ENG_SAMPLE
#define PAIRING_KEY_PRIV lt_sh0priv_eng_sample
#define PAIRING_KEY_PUB  lt_sh0pub_eng_sample
#define PAIRING_KEY_SLOT TR01_PAIRING_KEY_SLOT_INDEX_0

#elif CDC_PAIRING_KEY == CDC_PAIRING_KEY_CUSTOM
/* Provides cdc_pairing_key_priv[], cdc_pairing_key_pub[] and optionally
 * CDC_PAIRING_KEY_SLOT. Not committed; copy pairing_key_custom.h.example. */
#include "pairing_key_custom.h"
#define PAIRING_KEY_PRIV cdc_pairing_key_priv
#define PAIRING_KEY_PUB  cdc_pairing_key_pub
#ifndef CDC_PAIRING_KEY_SLOT
#define CDC_PAIRING_KEY_SLOT TR01_PAIRING_KEY_SLOT_INDEX_0
#endif
#define PAIRING_KEY_SLOT CDC_PAIRING_KEY_SLOT

#else
#error "Invalid CDC_PAIRING_KEY: use CDC_PAIRING_KEY_PROD0, _ENG_SAMPLE or _CUSTOM"
#endif
