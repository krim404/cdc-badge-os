/**
 * \file
 * \brief Yubico HMAC-SHA1 challenge-response state machine over HID feature
 *        reports: CRC16, frame (de)assembly, touch gate and response framing.
 *
 * Protocol references:
 *  - yubikey-personalization ykcore/ykdef.h (frame_st, slot/flag constants).
 *  - yubikey-personalization ykcore/ykcore.c (yk_write_to_key /
 *    yk_read_response_from_key feature-report wire protocol).
 *  - yubico-c ykcrc.c (CRC16 ISO13239, poly 0x8408, residual 0xF0B8).
 *
 * Cross-task safety: \ref onSetReport / \ref onGetReport run on the TinyUSB
 * task and only buffer state; \ref tick runs on the main task and is the only
 * place that computes crypto, drives the E-Paper confirm UI, or touches the
 * ServiceRegistry. This mirrors mod_2fa's BLE CR transport discipline.
 */

#include "mod_otphid/OtpHidCr.h"
#include "mod_otphid/OtpHidConstants.h"
#include "cdc_core/ServiceRegistry.h"
#include "cdc_core/IChallengeResponder.h"
#include "cdc_core/Raii.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

static const char* TAG = "OTPHID_CR";

namespace cdc::mod_otphid {

// Yubico OTP HID frame constants (ykdef.h).
static constexpr uint8_t SLOT_DATA_SIZE = 64;   // frame_st.payload
static constexpr uint8_t FRAME_SIZE = 70;       // payload(64) + slot(1) + crc(2) + filler(3)
static constexpr uint8_t SLOT_CHAL_HMAC2 = 0x38;
static constexpr uint8_t SLOT_WRITE_FLAG = 0x80;
static constexpr uint8_t RESP_PENDING_FLAG = 0x40;
static constexpr uint8_t SEQ_MASK = 0x1F;       // low 5 bits = readback sequence
static constexpr uint8_t SHA1_DIGEST_SIZE = 20;

// YubiKey status structure (ykdef.h status_st), little-endian on the wire.
// CONFIG2 marks the touch-configured slot 2 the host probes by default.
static constexpr uint8_t YK_VERSION_MAJOR = 2;
static constexpr uint8_t YK_VERSION_MINOR = 4;
static constexpr uint8_t YK_VERSION_BUILD = 0;
static constexpr uint8_t CONFIG1_VALID = 0x01;
static constexpr uint8_t CONFIG2_VALID = 0x02;
static constexpr uint8_t CONFIG2_TOUCH = 0x08;

// Response on the wire: 20-byte HMAC-SHA1 digest + 2-byte CRC16 (ykcore.c).
static constexpr uint8_t RESPONSE_SIZE = SHA1_DIGEST_SIZE + 2;
static constexpr uint8_t DATA_BYTES_PER_REPORT = FEATURE_RPT_SIZE - 1;  // 7

/**
 * \brief CR exchange phase, advanced by the SET/GET callbacks and the tick.
 */
enum class Phase : uint8_t {
    IDLE,      ///< No exchange in progress; GET returns the status frame.
    PENDING,   ///< Challenge assembled; tick must compute the response.
    WAITING,   ///< Computed but withheld for touch; GET reports "pending".
    READY,     ///< Response framed; GET streams the sequenced readback.
};

EXT_RAM_BSS_ATTR static uint8_t s_challenge[SLOT_DATA_SIZE] = {};
EXT_RAM_BSS_ATTR static uint8_t s_response[RESPONSE_SIZE] = {};
static uint8_t s_frame[FRAME_SIZE] = {};

static Phase s_phase = Phase::IDLE;
static uint8_t s_readSeq = 0;          // Next readback sequence number (1-based).
static SemaphoreHandle_t s_mutex = nullptr;

/**
 * \brief Lazily creates the state mutex (idempotent).
 */
static SemaphoreHandle_t ensureMutex() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex;
}

/**
 * \brief Fills an 8-byte feature frame with the emulated YubiKey status.
 * \param buffer Destination, at least FEATURE_RPT_SIZE bytes.
 */
static void fillStatusFrame(uint8_t* buffer) {
    struct __attribute__((packed)) YkStatus {
        uint8_t versionMajor;
        uint8_t versionMinor;
        uint8_t versionBuild;
        uint8_t pgmSeq;
        uint16_t touchLevel;
    } st = {};
    st.versionMajor = YK_VERSION_MAJOR;
    st.versionMinor = YK_VERSION_MINOR;
    st.versionBuild = YK_VERSION_BUILD;
    st.pgmSeq = 1;
    st.touchLevel = static_cast<uint16_t>(CONFIG1_VALID | CONFIG2_VALID | CONFIG2_TOUCH);

    memset(buffer, 0, FEATURE_RPT_SIZE);
    memcpy(buffer, &st, sizeof(st));
}

uint16_t OtpHidCr::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            int lsb = crc & 1;
            crc >>= 1;
            if (lsb) crc ^= 0x8408;
        }
    }
    return crc;
}

OtpHidCr& OtpHidCr::instance() {
    static OtpHidCr inst;
    return inst;
}

void OtpHidCr::reset() {
    core::MutexGuard guard(ensureMutex());
    s_phase = Phase::IDLE;
    s_readSeq = 0;
    memset(s_frame, 0, sizeof(s_frame));
    memset(s_challenge, 0, sizeof(s_challenge));
    memset(s_response, 0, sizeof(s_response));
}

void OtpHidCr::onSetReport(uint8_t const* buffer, uint16_t bufsize) {
    if (!buffer || bufsize < FEATURE_RPT_SIZE) return;

    // The host writes the 70-byte frame as 8-byte reports: 7 data bytes plus a
    // sequence/flag byte with SLOT_WRITE_FLAG set and the low 7 bits the block
    // index. A dummy report (no write flag) resets the device; ignore it.
    uint8_t flagByte = buffer[FEATURE_RPT_SIZE - 1];
    if ((flagByte & SLOT_WRITE_FLAG) == 0) {
        reset();
        return;
    }

    uint8_t seq = static_cast<uint8_t>(flagByte & 0x7F);
    size_t offset = static_cast<size_t>(seq) * DATA_BYTES_PER_REPORT;
    if (offset >= FRAME_SIZE) return;

    core::MutexGuard guard(ensureMutex());
    // A new transfer (block 0) supersedes any half-assembled or stale frame.
    if (seq == 0) {
        memset(s_frame, 0, sizeof(s_frame));
        if (s_phase != Phase::IDLE) {
            s_phase = Phase::IDLE;
            s_readSeq = 0;
        }
    }

    size_t avail = FRAME_SIZE - offset;
    size_t n = avail < DATA_BYTES_PER_REPORT ? avail : DATA_BYTES_PER_REPORT;
    memcpy(s_frame + offset, buffer, n);

    // The final block (covering the slot/CRC tail) completes the frame.
    if (offset + DATA_BYTES_PER_REPORT < FRAME_SIZE) return;

    // The frame CRC16 covers the 64-byte payload only and is stored little-
    // endian after the slot byte (payload[64], slot, crc, filler). Validate it
    // against a freshly computed payload CRC (ykpers yk_write_to_key).
    uint16_t txCrc = static_cast<uint16_t>(s_frame[SLOT_DATA_SIZE + 1]) |
                     static_cast<uint16_t>(s_frame[SLOT_DATA_SIZE + 2] << 8);
    if (crc16(s_frame, SLOT_DATA_SIZE) != txCrc) {
        LOG_W(TAG, "CR frame CRC mismatch");
        s_phase = Phase::IDLE;
        return;
    }
    if (s_frame[SLOT_DATA_SIZE] != SLOT_CHAL_HMAC2) {
        LOG_W(TAG, "CR frame slot 0x%02x not slot 2", s_frame[SLOT_DATA_SIZE]);
        s_phase = Phase::IDLE;
        return;
    }

    memcpy(s_challenge, s_frame, SLOT_DATA_SIZE);
    s_phase = Phase::PENDING;
}

uint16_t OtpHidCr::onGetReport(uint8_t* buffer, uint16_t reqlen) {
    if (!buffer || reqlen < FEATURE_RPT_SIZE) return 0;

    core::MutexGuard guard(ensureMutex());

    switch (s_phase) {
        case Phase::PENDING:
        case Phase::WAITING: {
            // Busy: report the pending flag with sequence 0 so the host keeps
            // polling (mirrors a real key awaiting computation/touch).
            memset(buffer, 0, FEATURE_RPT_SIZE);
            buffer[FEATURE_RPT_SIZE - 1] = RESP_PENDING_FLAG;
            return FEATURE_RPT_SIZE;
        }
        case Phase::READY: {
            size_t offset = static_cast<size_t>(s_readSeq) * DATA_BYTES_PER_REPORT;
            if (offset >= RESPONSE_SIZE) {
                // Transfer complete: sequence wraps to 0 (no pending flag) and
                // the state returns to idle for the next exchange.
                memset(buffer, 0, FEATURE_RPT_SIZE);
                s_phase = Phase::IDLE;
                s_readSeq = 0;
                return FEATURE_RPT_SIZE;
            }
            size_t avail = RESPONSE_SIZE - offset;
            size_t n = avail < DATA_BYTES_PER_REPORT ? avail : DATA_BYTES_PER_REPORT;
            memset(buffer, 0, FEATURE_RPT_SIZE);
            memcpy(buffer, s_response + offset, n);
            s_readSeq++;
            buffer[FEATURE_RPT_SIZE - 1] =
                static_cast<uint8_t>(RESP_PENDING_FLAG | (s_readSeq & SEQ_MASK));
            return FEATURE_RPT_SIZE;
        }
        case Phase::IDLE:
        default:
            fillStatusFrame(buffer);
            return FEATURE_RPT_SIZE;
    }
}

void OtpHidCr::deliverConfirmedResponse() {
    core::MutexGuard guard(ensureMutex());
    if (s_phase != Phase::WAITING) return;
    s_readSeq = 0;
    s_phase = Phase::READY;
    LOG_I(TAG, "CR response ready (%u bytes)", static_cast<unsigned>(RESPONSE_SIZE));
}

void OtpHidCr::onTouchConfirm(void* userData) {
    (void)userData;
    instance().deliverConfirmedResponse();
}

void OtpHidCr::onTouchCancel(void* userData) {
    (void)userData;
    core::MutexGuard guard(ensureMutex());
    LOG_W(TAG, "CR request declined by user");
    s_phase = Phase::IDLE;
    s_readSeq = 0;
}

void OtpHidCr::tick(uint32_t nowMs) {
    (void)nowMs;

    uint8_t challenge[SLOT_DATA_SIZE] = {};
    {
        core::MutexGuard guard(ensureMutex());
        if (s_phase != Phase::PENDING) return;
        memcpy(challenge, s_challenge, sizeof(challenge));
    }

    auto* responder = core::ServiceRegistry::instance()
                          .request<core::IChallengeResponder>(
                              core::ServiceType::CHALLENGE_RESPONDER);
    if (!responder) {
        LOG_W(TAG, "No challenge responder service");
        core::MutexGuard guard(ensureMutex());
        s_phase = Phase::IDLE;
        return;
    }

    // KeePassXC uses fixed 64-byte input: HMAC the full payload. The Yubico
    // OTP-HID protocol is SHA1-only, so the designated entry must yield exactly
    // a 20-byte digest; anything else (e.g. a SHA256 entry) is rejected here.
    uint8_t digest[core::IChallengeResponder::MAX_RESPONSE_LEN] = {};
    bool touchRequired = true;
    int rc = responder->challengeResponseUsbSlot(challenge, SLOT_DATA_SIZE, digest,
                                                 &touchRequired);
    if (rc != SHA1_DIGEST_SIZE) {
        LOG_W(TAG, "USB-CR compute failed or not SHA1 (rc=%d)", rc);
        core::MutexGuard guard(ensureMutex());
        s_phase = Phase::IDLE;
        return;
    }

    {
        core::MutexGuard guard(ensureMutex());
        if (s_phase != Phase::PENDING) return;  // Cancelled meanwhile.
        memcpy(s_response, digest, SHA1_DIGEST_SIZE);
        // Append the one's-complement CRC16 little-endian so the host's residual
        // check (crc16 over digest+crc == 0xF0B8) succeeds (ISO13239).
        uint16_t crc = static_cast<uint16_t>(~crc16(s_response, SHA1_DIGEST_SIZE));
        s_response[SHA1_DIGEST_SIZE] = static_cast<uint8_t>(crc & 0xFF);
        s_response[SHA1_DIGEST_SIZE + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        s_phase = Phase::WAITING;
        s_readSeq = 0;
    }

    if (touchRequired) {
        ui::showConfirm(ui::tr("mod_otphid.cr_confirm"), onTouchConfirm, onTouchCancel,
                        ui::ConfirmView::Icon::QUESTION, nullptr);
    } else {
        deliverConfirmedResponse();
    }
}

} // namespace cdc::mod_otphid
