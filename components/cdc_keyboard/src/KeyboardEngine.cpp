/**
 * \file
 * \brief Transport-agnostic keyboard typing engine.
 */

#include "cdc_keyboard/KeyboardEngine.h"
#include "cdc_keyboard/KeyboardLayout.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

namespace cdc::keyboard {

// ============================================================================
// Report emission
// ============================================================================

bool KeyboardEngine::sendKeyReport(uint8_t modifier, uint8_t keycode) {
    uint8_t one[1] = { keycode };
    return sink_.sendKeyReport(modifier, one, keycode == 0 ? 0 : 1);
}

bool KeyboardEngine::releaseAllKeys() {
    return sink_.sendKeyReport(0, nullptr, 0);
}

// ============================================================================
// Typing
// ============================================================================

bool KeyboardEngine::typeString(const char* text, uint16_t delayMs) {
    if (!text || !sink_.isConnected()) return false;
    if (busy_) return false;

    busy_ = true;
    cancelRequested_ = false;

    const char* p = text;
    while (*p && !cancelRequested_) {
        uint32_t codepoint;
        int bytes = utf8ToCodepoint(p, &codepoint);
        if (bytes <= 0) {
            p++;
            continue;
        }

        if (codepoint < 128) {
            typeAsciiChar(static_cast<char>(codepoint));
        } else {
            typeUnicodeChar(codepoint);
        }

        p += bytes;

        if (delayMs > 0 && *p) {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
    }

    releaseAllKeys();
    busy_ = false;
    return !cancelRequested_;
}

bool KeyboardEngine::typeChar(char c) {
    if (!sink_.isConnected()) return false;
    return typeAsciiChar(c);
}

bool KeyboardEngine::typeAsciiChar(char c) {
    KeyMapping mapping = getKeyMapping(c);
    if (mapping.keycode == KeyCode::KEY_NONE) {
        return false;
    }

    if (!sendKeyReport(mapping.modifier, mapping.keycode)) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    if (!releaseAllKeys()) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

bool KeyboardEngine::typeUnicodeChar(uint32_t codepoint) {
    switch (unicodeMethod_) {
        case UnicodeMethod::WINDOWS:
            return typeWindowsUnicode(codepoint);
        case UnicodeMethod::LINUX:
            return typeLinuxUnicode(codepoint);
        case UnicodeMethod::MACOS:
            return typeMacOsUnicode(codepoint);
        case UnicodeMethod::ASCII_ONLY:
        default:
            return typeAsciiFallback(codepoint);
    }
}

bool KeyboardEngine::typeWindowsUnicode(uint32_t codepoint) {
    sendKeyReport(Modifier::LEFT_ALT, KeyCode::KEY_NONE);
    vTaskDelay(pdMS_TO_TICKS(20));

    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)codepoint);

    for (const char* p = buf; *p; p++) {
        uint8_t kp = getNumpadKeycode(*p);
        if (kp != KeyCode::KEY_NONE) {
            sendKeyReport(Modifier::LEFT_ALT, kp);
            vTaskDelay(pdMS_TO_TICKS(20));
            sendKeyReport(Modifier::LEFT_ALT, KeyCode::KEY_NONE);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    releaseAllKeys();
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

bool KeyboardEngine::typeLinuxUnicode(uint32_t codepoint) {
    sendKeyReport(Modifier::LEFT_CTRL | Modifier::LEFT_SHIFT, KeyCode::KEY_U);
    vTaskDelay(pdMS_TO_TICKS(20));
    releaseAllKeys();
    vTaskDelay(pdMS_TO_TICKS(20));

    char buf[16];
    snprintf(buf, sizeof(buf), "%lx", (unsigned long)codepoint);

    for (const char* p = buf; *p; p++) {
        KeyMapping mapping = getKeyMapping(*p);
        if (mapping.keycode != KeyCode::KEY_NONE) {
            sendKeyReport(mapping.modifier, mapping.keycode);
            vTaskDelay(pdMS_TO_TICKS(20));
            releaseAllKeys();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    sendKeyReport(Modifier::NONE, KeyCode::KEY_ENTER);
    vTaskDelay(pdMS_TO_TICKS(20));
    releaseAllKeys();
    vTaskDelay(pdMS_TO_TICKS(20));

    return true;
}

bool KeyboardEngine::typeMacOsUnicode(uint32_t codepoint) {
    return typeAsciiFallback(codepoint);
}

bool KeyboardEngine::typeAsciiFallback(uint32_t codepoint) {
    switch (codepoint) {
        case 0x00E4: typeAsciiChar('a'); typeAsciiChar('e'); return true;
        case 0x00F6: typeAsciiChar('o'); typeAsciiChar('e'); return true;
        case 0x00FC: typeAsciiChar('u'); typeAsciiChar('e'); return true;
        case 0x00C4: typeAsciiChar('A'); typeAsciiChar('e'); return true;
        case 0x00D6: typeAsciiChar('O'); typeAsciiChar('e'); return true;
        case 0x00DC: typeAsciiChar('U'); typeAsciiChar('e'); return true;
        case 0x00DF: typeAsciiChar('s'); typeAsciiChar('s'); return true;
        case 0x20AC: typeAsciiChar('E'); typeAsciiChar('U'); typeAsciiChar('R'); return true;
        default: return true;
    }
}

// ============================================================================
// UTF-8 parsing
// ============================================================================

int KeyboardEngine::utf8ToCodepoint(const char* utf8, uint32_t* codepoint) {
    if (!utf8 || !codepoint) return 0;

    uint8_t b0 = static_cast<uint8_t>(utf8[0]);

    if ((b0 & 0x80) == 0) {
        *codepoint = b0;
        return 1;
    }

    if ((b0 & 0xE0) == 0xC0) {
        if (!utf8[1]) return 0;
        *codepoint = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(utf8[1]) & 0x3F);
        return 2;
    }

    if ((b0 & 0xF0) == 0xE0) {
        if (!utf8[1] || !utf8[2]) return 0;
        *codepoint = ((b0 & 0x0F) << 12) |
                     ((static_cast<uint8_t>(utf8[1]) & 0x3F) << 6) |
                     (static_cast<uint8_t>(utf8[2]) & 0x3F);
        return 3;
    }

    if ((b0 & 0xF8) == 0xF0) {
        if (!utf8[1] || !utf8[2] || !utf8[3]) return 0;
        *codepoint = ((b0 & 0x07) << 18) |
                     ((static_cast<uint8_t>(utf8[1]) & 0x3F) << 12) |
                     ((static_cast<uint8_t>(utf8[2]) & 0x3F) << 6) |
                     (static_cast<uint8_t>(utf8[3]) & 0x3F);
        return 4;
    }

    return 0;
}

} // namespace cdc::keyboard
