#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::core {

/**
 * \brief Challenge-response provider interface.
 *
 * Lets transport modules (USB OTP-HID, BLE GATT) obtain a raw HMAC response for
 * a named credential without depending on the 2FA module that computes it. The
 * 2FA module registers an implementation via
 * `ServiceRegistry::provide(ServiceType::CHALLENGE_RESPONDER, this)`; consumers
 * resolve it with `request<IChallengeResponder>(ServiceType::CHALLENGE_RESPONDER)`.
 */
class IChallengeResponder {
public:
    /// Largest possible raw HMAC response (SHA256). Callers size \p out to this.
    static constexpr size_t MAX_RESPONSE_LEN = 32;

    virtual ~IChallengeResponder() = default;

    /**
     * \brief Computes the raw HMAC challenge-response for a named CR entry.
     *
     * Looks up the credential by name, computes `HMAC(secret, challenge)` with
     * the entry's algorithm (SHA1 or SHA256), and writes the full, untruncated
     * digest to \p out. The caller must satisfy any touch/PIN gate separately;
     * this call performs no user confirmation.
     *
     * \param entryName Null-terminated credential name to look up.
     * \param challenge Challenge bytes.
     * \param clen Challenge length in bytes.
     * \param out Output buffer, must be at least \ref MAX_RESPONSE_LEN bytes.
     * \return Response length in bytes (20 for SHA1, 32 for SHA256), or `-1` on
     *         error or when no matching CR entry exists.
     */
    virtual int challengeResponse(const char* entryName, const uint8_t* challenge,
                                  size_t clen, uint8_t* out) = 0;

    /**
     * \brief Computes the raw HMAC response for the designated USB-CR slot entry.
     *
     * The USB OTP-HID transport (YubiKey slot 2) does not name a credential: it
     * answers from the single entry the user marked as the USB-CR slot. This
     * resolves that entry and computes `HMAC(secret, challenge)` (SHA1 over this
     * path), reporting whether an on-device touch confirmation is required. The
     * caller withholds the response until the touch gate is satisfied.
     *
     * \param challenge Challenge bytes.
     * \param clen Challenge length in bytes.
     * \param out Output buffer, must be at least \ref MAX_RESPONSE_LEN bytes.
     * \param touchRequiredOut Optional; receives the entry's touch-required flag.
     * \return Response length in bytes, or `-1` when no entry is designated or
     *         computation fails.
     */
    virtual int challengeResponseUsbSlot(const uint8_t* challenge, size_t clen,
                                         uint8_t* out, bool* touchRequiredOut) = 0;
};

} // namespace cdc::core
