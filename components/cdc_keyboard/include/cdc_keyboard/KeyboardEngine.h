#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::keyboard {

/**
 * \brief Unicode input method for non-ASCII characters.
 *
 * Selects how the engine emits characters outside the ASCII range, since the
 * HID keyboard report can only carry keycodes for a US-QWERTY layout.
 */
enum class UnicodeMethod : uint8_t {
    ASCII_ONLY = 0,  // Fallback: ae, oe, ue (works everywhere)
    WINDOWS,         // Alt + Numpad codes
    LINUX,           // Ctrl+Shift+U + hex + Enter
    MACOS,           // Option + key sequences (limited)
};

/**
 * \brief Transport sink for finished HID keyboard reports.
 *
 * The engine produces an ordered sequence of 8-byte boot-keyboard reports and
 * pushes each one through this sink. The transport (BLE GATT notification, USB
 * HID interrupt IN, etc.) implements delivery; the engine stays transport-agnostic.
 */
class IKeyReportSink {
public:
    IKeyReportSink() = default;
    virtual ~IKeyReportSink() = default;

    IKeyReportSink(const IKeyReportSink&) = delete;
    IKeyReportSink& operator=(const IKeyReportSink&) = delete;

    /**
     * \brief Deliver a single multi-key HID report.
     * \param modifier Modifier bitmask (Modifier flags).
     * \param keycodes Array of up to 6 keycodes; unused slots must be 0.
     * \param numKeys Number of valid entries in keycodes (0..6).
     * \return true if the report was delivered.
     */
    virtual bool sendKeyReport(uint8_t modifier, const uint8_t* keycodes,
                               uint8_t numKeys) = 0;

    /**
     * \brief Reports whether the transport is connected and able to type.
     * \return true if reports can currently be delivered.
     */
    virtual bool isConnected() const = 0;
};

/**
 * \brief Transport-agnostic keyboard typing engine.
 *
 * Turns a UTF-8/CP437 string into an ordered sequence of HID keyboard reports
 * (key press + release, with inter-key delays) and pushes them through an
 * IKeyReportSink. Owns the layout lookup, the selected Unicode method, the
 * UTF-8 decoder and the ASCII/Windows/Linux/macOS emission strategies. It holds
 * no transport, BLE or USB state.
 */
class KeyboardEngine {
public:
    /**
     * \brief Constructs an engine bound to a report sink.
     * \param sink Transport sink that delivers the generated reports.
     */
    explicit KeyboardEngine(IKeyReportSink& sink) : sink_(sink) {}

    /**
     * \brief Selects the Unicode input method for non-ASCII characters.
     * \param method Method to use on subsequent typeString() calls.
     */
    void setUnicodeMethod(UnicodeMethod method) { unicodeMethod_ = method; }

    /**
     * \brief Returns the currently selected Unicode method.
     * \return Active Unicode method.
     */
    UnicodeMethod getUnicodeMethod() const { return unicodeMethod_; }

    /**
     * \brief Types a UTF-8 string by emitting the corresponding HID reports.
     *
     * Blocks for the duration of the typing sequence (inter-key delays). Returns
     * early if the transport disconnects or cancel() is requested.
     * \param text Null-terminated UTF-8 text to type.
     * \param delayMs Delay between characters in milliseconds.
     * \return true if the full string was typed without cancellation.
     */
    bool typeString(const char* text, uint16_t delayMs);

    /**
     * \brief Types a single ASCII character.
     * \param c ASCII character (0-127).
     * \return true if the character was emitted.
     */
    bool typeChar(char c);

    /**
     * \brief Requests cancellation of an in-progress typeString().
     *
     * Takes effect at the next character boundary.
     */
    void cancel() { cancelRequested_ = true; }

    /**
     * \brief Reports whether a typeString() call is currently running.
     * \return true while typing.
     */
    bool isBusy() const { return busy_; }

private:
    bool typeAsciiChar(char c);
    bool typeUnicodeChar(uint32_t codepoint);
    bool typeWindowsUnicode(uint32_t codepoint);
    bool typeLinuxUnicode(uint32_t codepoint);
    bool typeMacOsUnicode(uint32_t codepoint);
    bool typeAsciiFallback(uint32_t codepoint);

    bool sendKeyReport(uint8_t modifier, uint8_t keycode);
    bool releaseAllKeys();

    static int utf8ToCodepoint(const char* utf8, uint32_t* codepoint);

    IKeyReportSink& sink_;
    UnicodeMethod unicodeMethod_ = UnicodeMethod::ASCII_ONLY;
    bool busy_ = false;
    bool cancelRequested_ = false;
};

} // namespace cdc::keyboard
