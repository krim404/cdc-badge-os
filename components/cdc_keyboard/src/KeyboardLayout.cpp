#include "cdc_keyboard/KeyboardLayout.h"

namespace cdc::keyboard {

/**
 * ASCII to HID keycode mapping table (US QWERTY layout)
 * Index is ASCII code, value is {keycode, modifier}
 */
static const KeyMapping s_asciiMap[128] = {
    // 0x00-0x1F: Control characters (mostly not mappable)
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x00 NUL
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x01 SOH
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x02 STX
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x03 ETX
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x04 EOT
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x05 ENQ
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x06 ACK
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x07 BEL
    {KeyCode::KEY_BACKSPACE, Modifier::NONE},   // 0x08 BS
    {KeyCode::KEY_TAB, Modifier::NONE},         // 0x09 TAB
    {KeyCode::KEY_ENTER, Modifier::NONE},       // 0x0A LF (newline)
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x0B VT
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x0C FF
    {KeyCode::KEY_ENTER, Modifier::NONE},       // 0x0D CR
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x0E SO
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x0F SI
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x10 DLE
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x11 DC1
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x12 DC2
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x13 DC3
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x14 DC4
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x15 NAK
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x16 SYN
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x17 ETB
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x18 CAN
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x19 EM
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x1A SUB
    {KeyCode::KEY_ESCAPE, Modifier::NONE},      // 0x1B ESC
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x1C FS
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x1D GS
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x1E RS
    {KeyCode::KEY_NONE, Modifier::NONE},        // 0x1F US

    // 0x20-0x2F: Punctuation and symbols
    {KeyCode::KEY_SPACE, Modifier::NONE},       // 0x20 SPACE
    {KeyCode::KEY_1, Modifier::LEFT_SHIFT},     // 0x21 !
    {KeyCode::KEY_QUOTE, Modifier::LEFT_SHIFT}, // 0x22 "
    {KeyCode::KEY_3, Modifier::LEFT_SHIFT},     // 0x23 #
    {KeyCode::KEY_4, Modifier::LEFT_SHIFT},     // 0x24 $
    {KeyCode::KEY_5, Modifier::LEFT_SHIFT},     // 0x25 %
    {KeyCode::KEY_7, Modifier::LEFT_SHIFT},     // 0x26 &
    {KeyCode::KEY_QUOTE, Modifier::NONE},       // 0x27 '
    {KeyCode::KEY_9, Modifier::LEFT_SHIFT},     // 0x28 (
    {KeyCode::KEY_0, Modifier::LEFT_SHIFT},     // 0x29 )
    {KeyCode::KEY_8, Modifier::LEFT_SHIFT},     // 0x2A *
    {KeyCode::KEY_EQUAL, Modifier::LEFT_SHIFT}, // 0x2B +
    {KeyCode::KEY_COMMA, Modifier::NONE},       // 0x2C ,
    {KeyCode::KEY_MINUS, Modifier::NONE},       // 0x2D -
    {KeyCode::KEY_PERIOD, Modifier::NONE},      // 0x2E .
    {KeyCode::KEY_SLASH, Modifier::NONE},       // 0x2F /

    // 0x30-0x39: Digits
    {KeyCode::KEY_0, Modifier::NONE},           // 0x30 0
    {KeyCode::KEY_1, Modifier::NONE},           // 0x31 1
    {KeyCode::KEY_2, Modifier::NONE},           // 0x32 2
    {KeyCode::KEY_3, Modifier::NONE},           // 0x33 3
    {KeyCode::KEY_4, Modifier::NONE},           // 0x34 4
    {KeyCode::KEY_5, Modifier::NONE},           // 0x35 5
    {KeyCode::KEY_6, Modifier::NONE},           // 0x36 6
    {KeyCode::KEY_7, Modifier::NONE},           // 0x37 7
    {KeyCode::KEY_8, Modifier::NONE},           // 0x38 8
    {KeyCode::KEY_9, Modifier::NONE},           // 0x39 9

    // 0x3A-0x40: More symbols
    {KeyCode::KEY_SEMICOLON, Modifier::LEFT_SHIFT},  // 0x3A :
    {KeyCode::KEY_SEMICOLON, Modifier::NONE},        // 0x3B ;
    {KeyCode::KEY_COMMA, Modifier::LEFT_SHIFT},      // 0x3C <
    {KeyCode::KEY_EQUAL, Modifier::NONE},            // 0x3D =
    {KeyCode::KEY_PERIOD, Modifier::LEFT_SHIFT},     // 0x3E >
    {KeyCode::KEY_SLASH, Modifier::LEFT_SHIFT},      // 0x3F ?
    {KeyCode::KEY_2, Modifier::LEFT_SHIFT},          // 0x40 @

    // 0x41-0x5A: Uppercase letters
    {KeyCode::KEY_A, Modifier::LEFT_SHIFT},     // 0x41 A
    {KeyCode::KEY_B, Modifier::LEFT_SHIFT},     // 0x42 B
    {KeyCode::KEY_C, Modifier::LEFT_SHIFT},     // 0x43 C
    {KeyCode::KEY_D, Modifier::LEFT_SHIFT},     // 0x44 D
    {KeyCode::KEY_E, Modifier::LEFT_SHIFT},     // 0x45 E
    {KeyCode::KEY_F, Modifier::LEFT_SHIFT},     // 0x46 F
    {KeyCode::KEY_G, Modifier::LEFT_SHIFT},     // 0x47 G
    {KeyCode::KEY_H, Modifier::LEFT_SHIFT},     // 0x48 H
    {KeyCode::KEY_I, Modifier::LEFT_SHIFT},     // 0x49 I
    {KeyCode::KEY_J, Modifier::LEFT_SHIFT},     // 0x4A J
    {KeyCode::KEY_K, Modifier::LEFT_SHIFT},     // 0x4B K
    {KeyCode::KEY_L, Modifier::LEFT_SHIFT},     // 0x4C L
    {KeyCode::KEY_M, Modifier::LEFT_SHIFT},     // 0x4D M
    {KeyCode::KEY_N, Modifier::LEFT_SHIFT},     // 0x4E N
    {KeyCode::KEY_O, Modifier::LEFT_SHIFT},     // 0x4F O
    {KeyCode::KEY_P, Modifier::LEFT_SHIFT},     // 0x50 P
    {KeyCode::KEY_Q, Modifier::LEFT_SHIFT},     // 0x51 Q
    {KeyCode::KEY_R, Modifier::LEFT_SHIFT},     // 0x52 R
    {KeyCode::KEY_S, Modifier::LEFT_SHIFT},     // 0x53 S
    {KeyCode::KEY_T, Modifier::LEFT_SHIFT},     // 0x54 T
    {KeyCode::KEY_U, Modifier::LEFT_SHIFT},     // 0x55 U
    {KeyCode::KEY_V, Modifier::LEFT_SHIFT},     // 0x56 V
    {KeyCode::KEY_W, Modifier::LEFT_SHIFT},     // 0x57 W
    {KeyCode::KEY_X, Modifier::LEFT_SHIFT},     // 0x58 X
    {KeyCode::KEY_Y, Modifier::LEFT_SHIFT},     // 0x59 Y
    {KeyCode::KEY_Z, Modifier::LEFT_SHIFT},     // 0x5A Z

    // 0x5B-0x60: More symbols
    {KeyCode::KEY_LEFTBRACKET, Modifier::NONE},       // 0x5B [
    {KeyCode::KEY_BACKSLASH, Modifier::NONE},         // 0x5C backslash
    {KeyCode::KEY_RIGHTBRACKET, Modifier::NONE},      // 0x5D ]
    {KeyCode::KEY_6, Modifier::LEFT_SHIFT},           // 0x5E ^
    {KeyCode::KEY_MINUS, Modifier::LEFT_SHIFT},       // 0x5F _
    {KeyCode::KEY_GRAVE, Modifier::NONE},             // 0x60 `

    // 0x61-0x7A: Lowercase letters
    {KeyCode::KEY_A, Modifier::NONE},           // 0x61 a
    {KeyCode::KEY_B, Modifier::NONE},           // 0x62 b
    {KeyCode::KEY_C, Modifier::NONE},           // 0x63 c
    {KeyCode::KEY_D, Modifier::NONE},           // 0x64 d
    {KeyCode::KEY_E, Modifier::NONE},           // 0x65 e
    {KeyCode::KEY_F, Modifier::NONE},           // 0x66 f
    {KeyCode::KEY_G, Modifier::NONE},           // 0x67 g
    {KeyCode::KEY_H, Modifier::NONE},           // 0x68 h
    {KeyCode::KEY_I, Modifier::NONE},           // 0x69 i
    {KeyCode::KEY_J, Modifier::NONE},           // 0x6A j
    {KeyCode::KEY_K, Modifier::NONE},           // 0x6B k
    {KeyCode::KEY_L, Modifier::NONE},           // 0x6C l
    {KeyCode::KEY_M, Modifier::NONE},           // 0x6D m
    {KeyCode::KEY_N, Modifier::NONE},           // 0x6E n
    {KeyCode::KEY_O, Modifier::NONE},           // 0x6F o
    {KeyCode::KEY_P, Modifier::NONE},           // 0x70 p
    {KeyCode::KEY_Q, Modifier::NONE},           // 0x71 q
    {KeyCode::KEY_R, Modifier::NONE},           // 0x72 r
    {KeyCode::KEY_S, Modifier::NONE},           // 0x73 s
    {KeyCode::KEY_T, Modifier::NONE},           // 0x74 t
    {KeyCode::KEY_U, Modifier::NONE},           // 0x75 u
    {KeyCode::KEY_V, Modifier::NONE},           // 0x76 v
    {KeyCode::KEY_W, Modifier::NONE},           // 0x77 w
    {KeyCode::KEY_X, Modifier::NONE},           // 0x78 x
    {KeyCode::KEY_Y, Modifier::NONE},           // 0x79 y
    {KeyCode::KEY_Z, Modifier::NONE},           // 0x7A z

    // 0x7B-0x7F: More symbols
    {KeyCode::KEY_LEFTBRACKET, Modifier::LEFT_SHIFT},  // 0x7B {
    {KeyCode::KEY_BACKSLASH, Modifier::LEFT_SHIFT},    // 0x7C |
    {KeyCode::KEY_RIGHTBRACKET, Modifier::LEFT_SHIFT}, // 0x7D }
    {KeyCode::KEY_GRAVE, Modifier::LEFT_SHIFT},        // 0x7E ~
    {KeyCode::KEY_NONE, Modifier::NONE},               // 0x7F DEL
};

KeyMapping getKeyMapping(char c) {
    if (static_cast<unsigned char>(c) > 127) {
        return {KeyCode::KEY_NONE, Modifier::NONE};
    }
    return s_asciiMap[static_cast<uint8_t>(c)];
}

uint8_t getNumpadKeycode(char digit) {
    if (digit >= '0' && digit <= '9') {
        if (digit == '0') {
            return KeyCode::KEY_KP_0;
        }
        return KeyCode::KEY_KP_1 + (digit - '1');
    }
    return KeyCode::KEY_NONE;
}

} // namespace cdc::keyboard
