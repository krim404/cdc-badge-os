#pragma once

#include <cstdint>

namespace cdc::keyboard {

/**
 * USB HID Keyboard Keycodes (USB HID Usage Tables 1.12)
 * Section 10: Keyboard/Keypad Page (0x07)
 */
namespace KeyCode {
    // Letters (US QWERTY layout)
    constexpr uint8_t KEY_A = 0x04;
    constexpr uint8_t KEY_B = 0x05;
    constexpr uint8_t KEY_C = 0x06;
    constexpr uint8_t KEY_D = 0x07;
    constexpr uint8_t KEY_E = 0x08;
    constexpr uint8_t KEY_F = 0x09;
    constexpr uint8_t KEY_G = 0x0A;
    constexpr uint8_t KEY_H = 0x0B;
    constexpr uint8_t KEY_I = 0x0C;
    constexpr uint8_t KEY_J = 0x0D;
    constexpr uint8_t KEY_K = 0x0E;
    constexpr uint8_t KEY_L = 0x0F;
    constexpr uint8_t KEY_M = 0x10;
    constexpr uint8_t KEY_N = 0x11;
    constexpr uint8_t KEY_O = 0x12;
    constexpr uint8_t KEY_P = 0x13;
    constexpr uint8_t KEY_Q = 0x14;
    constexpr uint8_t KEY_R = 0x15;
    constexpr uint8_t KEY_S = 0x16;
    constexpr uint8_t KEY_T = 0x17;
    constexpr uint8_t KEY_U = 0x18;
    constexpr uint8_t KEY_V = 0x19;
    constexpr uint8_t KEY_W = 0x1A;
    constexpr uint8_t KEY_X = 0x1B;
    constexpr uint8_t KEY_Y = 0x1C;
    constexpr uint8_t KEY_Z = 0x1D;

    // Numbers (top row)
    constexpr uint8_t KEY_1 = 0x1E;
    constexpr uint8_t KEY_2 = 0x1F;
    constexpr uint8_t KEY_3 = 0x20;
    constexpr uint8_t KEY_4 = 0x21;
    constexpr uint8_t KEY_5 = 0x22;
    constexpr uint8_t KEY_6 = 0x23;
    constexpr uint8_t KEY_7 = 0x24;
    constexpr uint8_t KEY_8 = 0x25;
    constexpr uint8_t KEY_9 = 0x26;
    constexpr uint8_t KEY_0 = 0x27;

    // Control keys
    constexpr uint8_t KEY_ENTER = 0x28;
    constexpr uint8_t KEY_ESCAPE = 0x29;
    constexpr uint8_t KEY_BACKSPACE = 0x2A;
    constexpr uint8_t KEY_TAB = 0x2B;
    constexpr uint8_t KEY_SPACE = 0x2C;

    // Symbols (US layout)
    constexpr uint8_t KEY_MINUS = 0x2D;          // - _
    constexpr uint8_t KEY_EQUAL = 0x2E;          // = +
    constexpr uint8_t KEY_LEFTBRACKET = 0x2F;    // [ {
    constexpr uint8_t KEY_RIGHTBRACKET = 0x30;   // ] }
    constexpr uint8_t KEY_BACKSLASH = 0x31;      // \ |
    constexpr uint8_t KEY_SEMICOLON = 0x33;      // ; :
    constexpr uint8_t KEY_QUOTE = 0x34;          // ' "
    constexpr uint8_t KEY_GRAVE = 0x35;          // ` ~
    constexpr uint8_t KEY_COMMA = 0x36;          // , <
    constexpr uint8_t KEY_PERIOD = 0x37;         // . >
    constexpr uint8_t KEY_SLASH = 0x38;          // / ?

    // Function keys
    constexpr uint8_t KEY_F1 = 0x3A;
    constexpr uint8_t KEY_F2 = 0x3B;
    constexpr uint8_t KEY_F3 = 0x3C;
    constexpr uint8_t KEY_F4 = 0x3D;
    constexpr uint8_t KEY_F5 = 0x3E;
    constexpr uint8_t KEY_F6 = 0x3F;
    constexpr uint8_t KEY_F7 = 0x40;
    constexpr uint8_t KEY_F8 = 0x41;
    constexpr uint8_t KEY_F9 = 0x42;
    constexpr uint8_t KEY_F10 = 0x43;
    constexpr uint8_t KEY_F11 = 0x44;
    constexpr uint8_t KEY_F12 = 0x45;

    // Navigation
    constexpr uint8_t KEY_INSERT = 0x49;
    constexpr uint8_t KEY_HOME = 0x4A;
    constexpr uint8_t KEY_PAGEUP = 0x4B;
    constexpr uint8_t KEY_DELETE = 0x4C;
    constexpr uint8_t KEY_END = 0x4D;
    constexpr uint8_t KEY_PAGEDOWN = 0x4E;
    constexpr uint8_t KEY_RIGHT = 0x4F;
    constexpr uint8_t KEY_LEFT = 0x50;
    constexpr uint8_t KEY_DOWN = 0x51;
    constexpr uint8_t KEY_UP = 0x52;

    // Numpad
    constexpr uint8_t KEY_NUMLOCK = 0x53;
    constexpr uint8_t KEY_KP_DIVIDE = 0x54;
    constexpr uint8_t KEY_KP_MULTIPLY = 0x55;
    constexpr uint8_t KEY_KP_MINUS = 0x56;
    constexpr uint8_t KEY_KP_PLUS = 0x57;
    constexpr uint8_t KEY_KP_ENTER = 0x58;
    constexpr uint8_t KEY_KP_1 = 0x59;
    constexpr uint8_t KEY_KP_2 = 0x5A;
    constexpr uint8_t KEY_KP_3 = 0x5B;
    constexpr uint8_t KEY_KP_4 = 0x5C;
    constexpr uint8_t KEY_KP_5 = 0x5D;
    constexpr uint8_t KEY_KP_6 = 0x5E;
    constexpr uint8_t KEY_KP_7 = 0x5F;
    constexpr uint8_t KEY_KP_8 = 0x60;
    constexpr uint8_t KEY_KP_9 = 0x61;
    constexpr uint8_t KEY_KP_0 = 0x62;
    constexpr uint8_t KEY_KP_PERIOD = 0x63;

    // No key pressed
    constexpr uint8_t KEY_NONE = 0x00;
}

/**
 * Modifier key bits
 */
namespace Modifier {
    constexpr uint8_t NONE = 0x00;
    constexpr uint8_t LEFT_CTRL = 0x01;
    constexpr uint8_t LEFT_SHIFT = 0x02;
    constexpr uint8_t LEFT_ALT = 0x04;
    constexpr uint8_t LEFT_GUI = 0x08;
    constexpr uint8_t RIGHT_CTRL = 0x10;
    constexpr uint8_t RIGHT_SHIFT = 0x20;
    constexpr uint8_t RIGHT_ALT = 0x40;
    constexpr uint8_t RIGHT_GUI = 0x80;
}

/**
 * Lookup result for character to keycode mapping
 */
struct KeyMapping {
    uint8_t keycode;
    uint8_t modifier;
};

/**
 * Get HID keycode and modifier for an ASCII character
 * \param c ASCII character (0-127)
 * \return KeyMapping with keycode and modifier, or {KEY_NONE, NONE} if not mappable
 */
KeyMapping getKeyMapping(char c);

/**
 * Get numpad keycode for a digit
 * \param digit Character '0'-'9'
 * \return Numpad keycode or KEY_NONE
 */
uint8_t getNumpadKeycode(char digit);

} // namespace cdc::keyboard
