#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OpenPGP password management (PW1/PW3). */
void pin_storage_openpgp_init(void);
bool pin_storage_openpgp_verify_pw1(const char *pin);
bool pin_storage_openpgp_verify_pw3(const char *pin);
bool pin_storage_openpgp_change_pw1(const char *new_pin);
bool pin_storage_openpgp_change_pw3(const char *new_pin);

/* OpenPGP KDF-DO path: PW1/PW3 references are host-supplied pre-hashes. */
bool pin_storage_openpgp_verify_pw1_raw(const uint8_t *data, size_t len);
bool pin_storage_openpgp_verify_pw3_raw(const uint8_t *data, size_t len);
bool pin_storage_openpgp_set_pw1_raw(const uint8_t *data, size_t len);
bool pin_storage_openpgp_set_pw3_raw(const uint8_t *data, size_t len);
uint8_t pin_storage_openpgp_pw1_retries(void);
uint8_t pin_storage_openpgp_pw3_retries(void);
void pin_storage_openpgp_reset_pw1_retries(void);
void pin_storage_openpgp_reset_pw3_retries(void);
bool pin_storage_openpgp_pw1_blocked(void);
bool pin_storage_openpgp_pw3_blocked(void);
bool pin_storage_openpgp_reset(void);

/* Badge PIN status (shared between modules). */
bool pin_storage_is_set(void);

/* FIDO2 ClientPIN support backed by the badge PIN hash. */
bool pin_storage_fido2_available(void);
bool pin_storage_get_fido2_hash(uint8_t* hash_out);
bool pin_storage_verify_fido2_hash(const uint8_t* hash_in);

/* CTAP2 setMinPINLength policy floor applied to the badge PIN. */
void pin_storage_set_min_pin_floor(uint8_t min_len);
uint8_t pin_storage_get_min_pin_floor(void);

#ifdef __cplusplus
}
#endif
