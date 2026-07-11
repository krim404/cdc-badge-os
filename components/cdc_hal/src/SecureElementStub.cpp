/**
 * Secure Element Stub
 * Temporary stub until Tropic01Element is adapted to new libtropic v3.x API
 */

#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"

static const char* TAG = "SE-Stub";

namespace cdc::hal {

/**
 * Stub Secure Element - returns errors for all operations
 */
class SecureElementStub : public ISecureElement {
public:
    /**
     * \brief Initializes stub secure element service.
     * \return Always `true`.
     */
    bool init() override {
        LOG_W(TAG, "SecureElement STUB - TROPIC01 not implemented yet");
        state_ = core::ServiceState::INITIALIZED;
        return true;
    }
    /**
     * \brief Starts stub service state.
     * \return Always `true`.
     */
    bool start() override { state_ = core::ServiceState::STARTED; return true; }
    /**
     * \brief Stops stub service state.
     */
    void stop() override { state_ = core::ServiceState::STOPPED; }
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "secure_element"; }

    bool sessionStart() override { return false; }
    /**
     * \brief Ends secure-element session (stub no-op).
     */
    void sessionEnd() override {}
    /**
     * \brief Returns secure-element session state.
     * \return Always `false` in stub.
     */
    bool isSessionActive() const override { return false; }
    /**
     * \brief Requests secure element sleep (stub no-op).
     */
    void sleep() override {}

    SeResult eccGenerate(uint8_t, EccCurve) override { return SeResult::NOT_SUPPORTED; }
    SeResult eccImport(uint8_t, const uint8_t*, EccCurve) override { return SeResult::NOT_SUPPORTED; }
    SeResult eccGetPublicKey(uint8_t, uint8_t*, EccCurve*) override { return SeResult::NOT_SUPPORTED; }
    SeResult eccDelete(uint8_t) override { return SeResult::NOT_SUPPORTED; }
    bool eccSlotUsed(uint8_t) const override { return false; }

    SeResult ecdsaSign(uint8_t, const uint8_t*, size_t, uint8_t*, size_t*) override {
        return SeResult::NOT_SUPPORTED;
    }
    SeResult ecdsaSignDigest(uint8_t, const uint8_t[32], uint8_t*, size_t*) override {
        return SeResult::NOT_SUPPORTED;
    }
    SeResult eddsaSign(uint8_t, const uint8_t*, size_t, uint8_t*) override {
        return SeResult::NOT_SUPPORTED;
    }

    SeResult rmemRead(uint16_t, uint8_t*, uint16_t, uint16_t*) override {
        return SeResult::NOT_SUPPORTED;
    }
    SeResult rmemWrite(uint16_t, const uint8_t*, uint16_t) override {
        return SeResult::NOT_SUPPORTED;
    }
    SeResult rmemErase(uint16_t) override { return SeResult::NOT_SUPPORTED; }
    bool rmemSlotUsed(uint16_t) const override { return false; }

    bool getRandom(uint8_t*, uint16_t) override { return false; }
    bool getRandomStrict(uint8_t*, uint16_t) override { return false; }
    bool getChipId(uint8_t*, uint8_t) override { return false; }
    bool getFwVersion(uint8_t[4], uint8_t[4]) override { return false; }

private:
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

static SecureElementStub g_secureElementStub;

/**
 * \brief Returns singleton secure-element stub instance.
 * \return Pointer to stub implementation.
 */
ISecureElement* getSecureElementInstance() {
    return &g_secureElementStub;
}

} // namespace cdc::hal
