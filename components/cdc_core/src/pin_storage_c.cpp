/**
 * \file
 * \brief C-shim exposing badge PIN operations to C-style modules.
 */

#include "cdc_core/pin_storage_c.h"
#include "cdc_core/PinManager.h"

extern "C" {

void pin_storage_openpgp_init(void) {
    cdc::core::PinManager::instance().init();
}

bool pin_storage_openpgp_verify_pw1(const char *pin) {
    return cdc::core::PinManager::instance().verifyPW1(pin);
}

bool pin_storage_openpgp_verify_pw3(const char *pin) {
    return cdc::core::PinManager::instance().verifyPW3(pin);
}

bool pin_storage_openpgp_change_pw1(const char *new_pin) {
    return cdc::core::PinManager::instance().setPW1(new_pin);
}

bool pin_storage_openpgp_change_pw3(const char *new_pin) {
    return cdc::core::PinManager::instance().setPW3(new_pin);
}

bool pin_storage_openpgp_verify_pw1_raw(const uint8_t *data, size_t len) {
    return cdc::core::PinManager::instance().verifyPW1Raw(data, len);
}

bool pin_storage_openpgp_verify_pw3_raw(const uint8_t *data, size_t len) {
    return cdc::core::PinManager::instance().verifyPW3Raw(data, len);
}

bool pin_storage_openpgp_set_pw1_raw(const uint8_t *data, size_t len) {
    return cdc::core::PinManager::instance().setPW1Raw(data, len);
}

bool pin_storage_openpgp_set_pw3_raw(const uint8_t *data, size_t len) {
    return cdc::core::PinManager::instance().setPW3Raw(data, len);
}

uint8_t pin_storage_openpgp_pw1_retries(void) {
    return cdc::core::PinManager::instance().getPW1Retries();
}

uint8_t pin_storage_openpgp_pw3_retries(void) {
    return cdc::core::PinManager::instance().getPW3Retries();
}

void pin_storage_openpgp_reset_pw1_retries(void) {
    cdc::core::PinManager::instance().resetPW1Retries();
}

void pin_storage_openpgp_reset_pw3_retries(void) {
    cdc::core::PinManager::instance().resetPW3Retries();
}

bool pin_storage_openpgp_pw1_blocked(void) {
    return cdc::core::PinManager::instance().isPW1Blocked();
}

bool pin_storage_openpgp_pw3_blocked(void) {
    return cdc::core::PinManager::instance().isPW3Blocked();
}

bool pin_storage_openpgp_reset(void) {
    auto& pm = cdc::core::PinManager::instance();
    bool ok1 = pm.setPW1(cdc::core::PinManager::DEFAULT_PW1);
    bool ok3 = pm.setPW3(cdc::core::PinManager::DEFAULT_PW3);
    pm.resetPW1Retries();
    pm.resetPW3Retries();
    return ok1 && ok3;
}

bool pin_storage_is_set(void) {
    auto& pm = cdc::core::PinManager::instance();
    pm.init();
    return pm.isPinSet();
}

bool pin_storage_fido2_available(void) {
    auto& pm = cdc::core::PinManager::instance();
    pm.init();
    uint8_t hash[cdc::core::PinManager::BADGE_HASH_SIZE] = {};
    return pm.getBadgePinHash(hash);
}

bool pin_storage_get_fido2_hash(uint8_t* hash_out) {
    auto& pm = cdc::core::PinManager::instance();
    pm.init();
    return pm.getBadgePinHash(hash_out);
}

bool pin_storage_verify_fido2_hash(const uint8_t* hash_in) {
    auto& pm = cdc::core::PinManager::instance();
    pm.init();
    return pm.verifyBadgePinHash(hash_in);
}

void pin_storage_set_min_pin_floor(uint8_t min_len) {
    cdc::core::PinManager::instance().setMinPinLengthFloor(min_len);
}

uint8_t pin_storage_get_min_pin_floor(void) {
    return cdc::core::PinManager::instance().minPinLengthFloor();
}

} // extern "C"
