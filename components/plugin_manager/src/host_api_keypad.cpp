/**
 * \file host_api_keypad.cpp
 * \brief Direct keypad polling for plugins (thin wrapper over IKeypad).
 *
 * Maps the plugin ABI key codes (KEY_0..KEY_9 = 0..9, KEY_Y = 10, KEY_N = 11)
 * onto the HAL's cdc::hal::Key enum. All state lives in the keypad HAL.
 */

// IKeypad.h first: host_api.h defines KEY_* macros that would otherwise
// clobber the cdc::hal::Key enumerators during preprocessing.
#include "cdc_hal/IKeypad.h"
#include "plugin_manager/host_api.h"

namespace {

cdc::hal::Key code_to_key(uint8_t code) {
    using cdc::hal::Key;
    if (code <= 9) return static_cast<Key>('0' + code);
    if (code == 10) return Key::KEY_YES;
    if (code == 11) return Key::KEY_NO;
    return Key::KEY_NONE;
}

int key_to_code(cdc::hal::Key k) {
    char c = static_cast<char>(k);
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'Y') return 10;
    if (c == 'N') return 11;
    return -1;
}

}  // namespace

extern "C" {

bool host_key_pressed(uint8_t key)
{
    auto* kp = cdc::hal::getKeypadInstance();
    if (!kp) return false;
    cdc::hal::Key k = code_to_key(key);
    return k != cdc::hal::Key::KEY_NONE && kp->isKeyPressed(k);
}

int host_key_consume_next(uint8_t* out_key)
{
    if (!out_key) return HOST_ERR_INVALID_ARG;
    auto* kp = cdc::hal::getKeypadInstance();
    if (!kp) return HOST_ERR_GENERIC;
    cdc::hal::Key k = kp->getNextKey();
    if (k == cdc::hal::Key::KEY_NONE) return HOST_ERR_NOT_FOUND;
    int code = key_to_code(k);
    if (code < 0) return HOST_ERR_NOT_FOUND;
    *out_key = static_cast<uint8_t>(code);
    return HOST_OK;
}

}  // extern "C"
