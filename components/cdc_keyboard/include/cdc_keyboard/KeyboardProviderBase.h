#pragma once

#include "cdc_core/IKeyboardProvider.h"
#include "cdc_keyboard/KeyboardEngine.h"

namespace cdc::keyboard {

/**
 * \brief Shared transport base for HID keyboard provider modules.
 *
 * Owns the transport-agnostic KeyboardEngine, implements the typing half of
 * core::IKeyboardProvider (typeString/typeChar/isBusy/cancel) by delegating to
 * the engine, and persists the selected UnicodeMethod in a transport-supplied
 * NVS namespace. A concrete transport (BLE GATT, USB HID interrupt IN) derives
 * from this and provides only the report-delivery sink (sendKeyReport),
 * connection state (isConnected), status text and the discoverable hook.
 */
class KeyboardProviderBase : public core::IKeyboardProvider,
                             public IKeyReportSink {
public:
    /**
     * \brief Constructs the base bound to a transport NVS namespace.
     * \param nvsNamespace NVS namespace used to persist the Unicode method.
     */
    explicit KeyboardProviderBase(const char* nvsNamespace)
        : engine_(*this), nvsNamespace_(nvsNamespace) {}

    // IKeyboardProvider typing surface (delegated to the engine).
    bool typeString(const char* text, uint16_t delayMs = 50) override {
        return engine_.typeString(text, delayMs);
    }
    bool typeChar(char c) override { return engine_.typeChar(c); }
    bool isBusy() const override { return engine_.isBusy(); }
    void cancel() override { engine_.cancel(); }

    /**
     * \brief Selects the Unicode input method and persists it to NVS.
     * \param method Method applied to subsequent typeString() calls.
     */
    void setUnicodeMethod(UnicodeMethod method) {
        engine_.setUnicodeMethod(method);
        saveSettings();
    }

    /**
     * \brief Returns the currently selected Unicode input method.
     * \return Active Unicode method.
     */
    UnicodeMethod getUnicodeMethod() const { return engine_.getUnicodeMethod(); }

protected:
    /**
     * \brief Loads the persisted Unicode method from NVS into the engine.
     */
    void loadSettings();

    /**
     * \brief Persists the engine's Unicode method to NVS.
     */
    void saveSettings();

    KeyboardEngine engine_;

private:
    const char* nvsNamespace_;
};

} // namespace cdc::keyboard
