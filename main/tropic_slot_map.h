#pragma once

#include <cstdint>

// Compile-time TROPIC01 slot map (edit before build)
//
// You have 32 ECC slots and 512 R-Memory slots.
// Slot 0 (ECC and RMEM) is reserved.
//
// Syntax is always:
//   ECC_SLOT_<MODULENAME>_START / ECC_SLOT_<MODULENAME>_END
//   RMEM_SLOT_<MODULENAME>_START / RMEM_SLOT_<MODULENAME>_END
//
// Module names should match module runtime names (IModule::getName()).

namespace cdc::tropic_map {

static constexpr uint8_t ECC_SLOT_MIN = 0;
static constexpr uint8_t ECC_SLOT_MAX = 31;
static constexpr uint8_t ECC_SLOT_RESERVED = 0;

static constexpr uint16_t RMEM_SLOT_MIN = 0;
static constexpr uint16_t RMEM_SLOT_MAX = 511;
static constexpr uint16_t RMEM_SLOT_RESERVED = 0;
static constexpr uint16_t RMEM_SLOT_MIN_ALLOC = 1;

// Module IDs (must be unique, 0-254). 255 is reserved for UNKNOWN.
// MODULE_ID values are permanent on-device identifiers; never reassign them.
// Add new modules with new IDs only, never reuse a freed slot/ID.
#define MODULE_ID_MOD_SYSTEM 0
#define MODULE_ID_MOD_GPG 2
#define MODULE_ID_MOD_CA 3
#define MODULE_ID_MOD_FIDO2 4
#define MODULE_ID_MOD_2FA 5
#define MODULE_ID_MOD_PASSWORD 6
#define MODULE_ID_PLUGIN_POOL 7
#define MODULE_ID_MOD_GPG_RSA 8
#define MODULE_ID_MOD_PIV 9
#define MODULE_ID_UNKNOWN 255

// ECC slot ranges
#define ECC_SLOT_MOD_GPG_START 1
#define ECC_SLOT_MOD_GPG_END 3
#define ECC_SLOT_MOD_CA_START 4
#define ECC_SLOT_MOD_CA_END 4
// mod_piv: ECC 5 = PIV key 9A, 6 = 9C, 7 = reserved (9D is a software key in
// R-Mem, TROPIC01 has no ECDH), 8 = 9E.
#define ECC_SLOT_MOD_PIV_START 5
#define ECC_SLOT_MOD_PIV_END 8
#define ECC_SLOT_MOD_FIDO2_START 9
#define ECC_SLOT_MOD_FIDO2_END 30
// Plugin ECC pool (single reserved slot, the last physical ECC slot).
#define ECC_SLOT_MOD_PLUGINS_START 31
#define ECC_SLOT_MOD_PLUGINS_END 31

// RMEM slot ranges (must be >= RMEM_SLOT_MIN_ALLOC)
// mod_gpg owns 1-3 (slot 1 unused, slot 2 = DEC ECC key, slot 3 = AES key).
// mod_gpg_rsa owns 131-136: three RSA private-key blobs, 2 slots each (an
// RSA-4096 blob spans two slots). Carved from the 2FA tail (one slot) and the
// front of the oversized password pool (five slots).
#define RMEM_SLOT_MOD_GPG_START 1
#define RMEM_SLOT_MOD_GPG_END 3
#define RMEM_SLOT_MOD_CA_START 4
#define RMEM_SLOT_MOD_CA_END 4
// mod_piv: R-Mem 5 = piv_state (mgmt key, CHUID GUID, CCC id), 6 = reserved,
// 7 = 9D software key blob, 8 = reserved.
#define RMEM_SLOT_MOD_PIV_START 5
#define RMEM_SLOT_MOD_PIV_END 8
#define RMEM_SLOT_MOD_FIDO2_START 9
#define RMEM_SLOT_MOD_FIDO2_END 30
#define RMEM_SLOT_MOD_2FA_START 31
#define RMEM_SLOT_MOD_2FA_END 130
#define RMEM_SLOT_MOD_GPG_RSA_START 131
#define RMEM_SLOT_MOD_GPG_RSA_END 136
#define RMEM_SLOT_MOD_PASSWORD_START 137
#define RMEM_SLOT_MOD_PASSWORD_END 500
// Plugin pool. The named sub-slots inside this range are assigned dynamically
// at runtime, but the range itself is part of the central map so it is bounds-
// and overlap-validated and consumers fetch it via the TropicSlotMap API.
#define RMEM_SLOT_MOD_PLUGINS_START 501
#define RMEM_SLOT_MOD_PLUGINS_END 511

// Slot map entries (do not include reserved slots)
#define TROPIC_ECC_SLOT_MAP(X) \
    X("mod_gpg", MODULE_ID_MOD_GPG, ECC_SLOT_MOD_GPG_START, ECC_SLOT_MOD_GPG_END) \
    X("mod_ca", MODULE_ID_MOD_CA, ECC_SLOT_MOD_CA_START, ECC_SLOT_MOD_CA_END) \
    X("mod_piv", MODULE_ID_MOD_PIV, ECC_SLOT_MOD_PIV_START, ECC_SLOT_MOD_PIV_END) \
    X("mod_fido2", MODULE_ID_MOD_FIDO2, ECC_SLOT_MOD_FIDO2_START, ECC_SLOT_MOD_FIDO2_END) \
    X("mod_plugins", MODULE_ID_PLUGIN_POOL, ECC_SLOT_MOD_PLUGINS_START, ECC_SLOT_MOD_PLUGINS_END)

#define TROPIC_RMEM_SLOT_MAP(X) \
    X("mod_gpg", MODULE_ID_MOD_GPG, RMEM_SLOT_MOD_GPG_START, RMEM_SLOT_MOD_GPG_END) \
    X("mod_ca", MODULE_ID_MOD_CA, RMEM_SLOT_MOD_CA_START, RMEM_SLOT_MOD_CA_END) \
    X("mod_piv", MODULE_ID_MOD_PIV, RMEM_SLOT_MOD_PIV_START, RMEM_SLOT_MOD_PIV_END) \
    X("mod_fido2", MODULE_ID_MOD_FIDO2, RMEM_SLOT_MOD_FIDO2_START, RMEM_SLOT_MOD_FIDO2_END) \
    X("mod_2fa", MODULE_ID_MOD_2FA, RMEM_SLOT_MOD_2FA_START, RMEM_SLOT_MOD_2FA_END) \
    X("mod_gpg_rsa", MODULE_ID_MOD_GPG_RSA, RMEM_SLOT_MOD_GPG_RSA_START, RMEM_SLOT_MOD_GPG_RSA_END) \
    X("mod_password", MODULE_ID_MOD_PASSWORD, RMEM_SLOT_MOD_PASSWORD_START, RMEM_SLOT_MOD_PASSWORD_END) \
    X("mod_plugins", MODULE_ID_PLUGIN_POOL, RMEM_SLOT_MOD_PLUGINS_START, RMEM_SLOT_MOD_PLUGINS_END)

} // namespace cdc::tropic_map
