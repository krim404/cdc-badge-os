#pragma once

#include "cdc_msg/BeaconManager.h"
#include "cdc_msg/MessageHandlerRegistry.h"
#include "cdc_msg/MessageTypes.h"

#include "cdc_core/IService.h"
#include "cdc_core/Raii.h"
#include "cdc_hal/IBluetoothController.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cdc::msg {

/**
 * \brief Generic badge-to-badge BLE message transfer service.
 *
 * Headless core (no UI dependency). Sends a typed payload (MIME type + bytes)
 * to a nearby badge over a lightweight, MIME-typed GATT profile. The receiving
 * badge consents (the cdc_os_ui layer renders the prompt) and the payload is
 * delivered to a registered handler. The link is encrypted via an ephemeral
 * pairing (numeric comparison) that is forgotten after the transfer.
 *
 * Threading: NimBLE host-task callbacks only set flags / copy small data under
 * a short mutex tryLock; tick() runs on the main task, advances both state
 * machines and performs heavy work (delivery, NVS, teardown).
 */
class MessageTransfer : public cdc::core::IService {
public:
    static MessageTransfer& instance();

    MessageTransfer(const MessageTransfer&) = delete;
    MessageTransfer& operator=(const MessageTransfer&) = delete;

    // === Handler registry ===
    /// \brief Register a handler for an incoming MIME type. See MessageHandlerRegistry.
    bool registerHandler(const char* mime, const char* descKey, DeliverFn deliver);
    /// \brief Remove a previously registered handler.
    void unregisterHandler(const char* mime);
    /// \brief \return true if any handler (live or deferred) accepts \p mime.
    bool hasHandler(const char* mime);

    /// Answers the OFFER no-handler check for not-yet-loaded plugins
    /// (host-task safe). Returns true if some installed plugin handles \p mime.
    using CanHandleFn = std::function<bool(const char* mime)>;
    /// Delivers a completed payload whose MIME type only a deferred (unloaded)
    /// plugin handles. Invoked on the main task; the implementation must defer
    /// the actual plugin load + dispatch to the plugin task. Returns true if
    /// the payload was accepted for (async) delivery.
    using DeferredDeliverFn = std::function<bool(const uint8_t* data, uint32_t len,
                                                 const char* mime, const char* peerName)>;
    /// \brief Register the deferred-handler fallback (see CanHandleFn/DeferredDeliverFn).
    void setDeferredHandler(CanHandleFn canHandle, DeferredDeliverFn deliver);

    // === Send (central) ===
    /**
     * \brief Send a typed payload directly to a known peer (no picker).
     * Copies the payload into PSRAM and starts connecting. \param persistent
     * remembers the verified pairing for this runtime session so follow-up
     * sends to the same peer skip the numeric-comparison and consent prompts;
     * the bond is forgotten on reboot or at clean teardown. \return false if a
     * send is already in progress or arguments are invalid.
     */
    bool sendTo(const uint8_t addr[6], uint8_t addrType, const char* mime,
                const uint8_t* data, uint32_t len, bool persistent = false);
    /**
     * \brief Buffer a payload and request the interactive peer picker.
     * The cdc_os_ui layer renders the picker and then calls
     * confirmInteractiveTarget() or cancelSend(). \param persistent as in
     * sendTo(). \return false if busy/invalid.
     */
    bool beginInteractiveSend(const char* mime, const uint8_t* data, uint32_t len,
                              bool persistent = false);
    /// \brief UI poll: \return true once when a peer picker should be shown.
    bool takeInteractiveRequest();
    /// \brief UI selected a peer for the pending interactive send.
    bool confirmInteractiveTarget(const uint8_t addr[6], uint8_t addrType);
    /// \brief Cancel an in-flight or pending send.
    void cancelSend();
    SendState sendState() const { return send_.state; }
    bool sendBusy() const { return send_.state != SendState::Idle; }
    /// \brief \return bytes sent so far; \p outTotal receives the total.
    uint32_t sendProgress(uint32_t* outTotal) const;
    /// \brief \return last send failure reason (valid right after Failed).
    Reason sendReason() const { return send_.reason; }

    // === Discovery (read-only beacon scan + peer picker source) ===
    /// \brief Start discovery. \p durationMs 0 scans continuously until
    /// stopDiscovery(); \p keepAdvertising leaves the beacon advertising so two
    /// scanning badges still discover each other.
    bool startDiscovery(uint32_t durationMs = 6000, bool keepAdvertising = false);
    /// \brief Stop an ongoing (continuous) discovery scan.
    void stopDiscovery();
    bool discoveryDone() const;
    /// \brief Copy nearby message-capable peers into \p out. \return count.
    uint8_t getPeers(PeerInfo* out, uint8_t maxPeers);

    // === Receiver consent (peripheral) ===
    bool hasPendingConsent() const;
    /**
     * \brief Read the pending consent details for the prompt UI.
     * \return false if no consent is pending.
     */
    bool getPendingConsent(char* peerName, size_t nameSize, char* mime, size_t mimeSize,
                           const char** outDescKey, uint32_t* outSize) const;
    /// \brief Resolve the pending consent (main task).
    void respondConsent(bool accept);
    RecvState recvState() const { return recv_.state; }
    /// \brief \return bytes received so far; \p outTotal receives the total.
    uint32_t recvProgress(uint32_t* outTotal) const;

    /// \brief Final outcome of the most recent transfer, for a completion toast.
    struct TransferResult {
        bool   ok = false;
        bool   wasSend = false;
        Reason reason = Reason::None;
        char   mime[kMimeBufSize] = {};
        char   peerName[kNameBufSize] = {};
    };
    /// \brief Consume the latest transfer result (cleared on read).
    /// \return false if no fresh result is available.
    bool consumeResult(TransferResult* out);

    // === Beacon ===
    void setBeaconEnabled(bool enabled);
    bool isBeaconEnabled() const { return beacon_.isEnabled(); }
    bool isBeaconActive() const { return beacon_.isActive(); }
    void setBeaconName(const char* name) { beacon_.setName(name); }
    const char* getBeaconName() const { return beacon_.getName(); }

    // === IService ===
    bool init() override;
    bool start() override;
    void stop() override;
    cdc::core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "msg"; }

    /// \brief Drive both state machines and the beacon. Call from the main loop.
    void tick(uint32_t nowMs);

private:
    MessageTransfer() = default;

    cdc::hal::IBluetoothController* ble() const;

    /// \return true if a live handler or the deferred resolver accepts \p mime.
    bool anyHandler(const char* mime);

    // GATT server registration + listener (un)registration.
    bool registerService();
    void registerListeners();
    void removeListeners();

    // GATT server write callbacks (host task).
    int onControlWrite(uint16_t conn, const uint8_t* data, uint16_t len);
    int onDataWrite(uint16_t conn, const uint8_t* data, uint16_t len);

    // Shared BLE callbacks (host task).
    void onConnect(uint16_t conn);
    void onDisconnect(uint16_t conn, int reason);
    void onServiceDiscovered(uint16_t conn,
                             const cdc::hal::IBluetoothController::DiscoveredService* svc,
                             bool complete);
    void onNotification(uint16_t conn, uint16_t attr, const uint8_t* data, uint16_t len);
    void onWriteComplete(uint16_t conn, uint16_t attr, int status);
    void onEncChange(uint16_t conn, int status);

    // Status notifications (receiver -> sender).
    void notifyStatus(uint16_t conn, StatusOp op);
    void notifyStatusByte(uint16_t conn, StatusOp op, uint8_t arg);
    void notifyStatusU32(uint16_t conn, StatusOp op, uint32_t value);

    // Receiver helpers.
    void enqueueOffer(uint16_t conn, const char* mime, uint32_t totalLen, const char* name,
                      bool persist);
    void promoteNextOffer(uint32_t nowMs);
    void tickRecv(uint32_t nowMs);
    void teardownRecv(bool sendError, Reason reason);
    bool tryConsumePromptBudget(uint32_t nowMs);
    bool rateLimitOk(uint16_t conn, uint32_t nowMs);
    void dropQueuedForConn(uint16_t conn);

    // Sender helpers.
    void tickSend(uint32_t nowMs);
    void teardownSend(bool ok, Reason reason);
    void streamChunks();

    void resetRecv();
    void resetSend();
    void recordBond(uint16_t conn, const uint8_t addr[6], uint8_t addrType);
    void forgetBondForConn(uint16_t conn);
    void storeResult(bool ok, bool wasSend, Reason reason, const char* mime, const char* peerName);

    // Session-persistent pairings (kept across disconnects within one runtime
    // session; never across a reboot). The NVS ledger is crash-cleanup only.
    bool isSessionPeer(const uint8_t addr[6], uint8_t addrType) const;
    void rememberSessionPeer(const uint8_t addr[6], uint8_t addrType);
    void forgetAllSessionPeers();
    void persistSessionLedger();
    void cleanupStaleSessionBonds();

    uint16_t mtuPayload() const;

    cdc::core::ServiceState state_ = cdc::core::ServiceState::UNINITIALIZED;
    SemaphoreHandle_t mutex_ = nullptr;
    bool serviceRegistered_ = false;
    bool bootApplied_ = false;

    MessageHandlerRegistry registry_;
    BeaconManager beacon_;

    CanHandleFn       canHandle_;        ///< Deferred resolver (installed plugins).
    DeferredDeliverFn deferredDeliver_;  ///< Deferred delivery to a plugin handler.

    // GATT server handle for the Status (notify) characteristic.
    uint16_t statusValueHandle_ = 0;

    // BLE listener tokens.
    cdc::hal::ListenerToken tokConn_     = cdc::hal::INVALID_LISTENER;
    cdc::hal::ListenerToken tokDisconn_  = cdc::hal::INVALID_LISTENER;
    cdc::hal::ListenerToken tokSvcDisc_  = cdc::hal::INVALID_LISTENER;
    cdc::hal::ListenerToken tokNotify_   = cdc::hal::INVALID_LISTENER;
    cdc::hal::ListenerToken tokWrite_    = cdc::hal::INVALID_LISTENER;
    cdc::hal::ListenerToken tokEnc_      = cdc::hal::INVALID_LISTENER;

    // ---- Receiver active session ----
    struct RecvSession {
        RecvState state = RecvState::Idle;
        uint16_t  conn = 0xFFFF;
        char      mime[kMimeBufSize] = {};
        char      peerName[kNameBufSize] = {};
        const char* descKey = nullptr;
        uint32_t  totalLen = 0;
        uint32_t  received = 0;
        uint32_t  lastProgress = 0;
        uint32_t  stateStartMs = 0;
        uint32_t  expectedCrc = 0;
        bool      completeReceived = false;
        bool      persistent = false;  ///< OFFER requested a session-persistent pairing.
        bool      peerAddrValid = false;
        uint8_t   peerAddr[6] = {};
        uint8_t   peerAddrType = 0;
        // Cross-task signals consumed in tickRecv().
        volatile bool encOk = false;
        volatile bool failFlag = false;
        Reason        failReason = Reason::None;
        cdc::core::PsramUniquePtr<uint8_t> buf;
    } recv_;

    // ---- Pending inbound offers (FIFO) ----
    struct PendingOffer {
        bool        used = false;
        uint16_t    conn = 0xFFFF;
        char        mime[kMimeBufSize] = {};
        char        peerName[kNameBufSize] = {};
        const char* descKey = nullptr;
        uint32_t    totalLen = 0;
        uint32_t    enqueuedMs = 0;
        bool        persist = false;  ///< OFFER requested a session-persistent pairing.
    };
    std::array<PendingOffer, kOfferQueueDepth> queue_{};

    // ---- DoS: global prompt budget (address-independent) ----
    uint32_t promptWindowStartMs_ = 0;
    uint8_t  promptsInWindow_ = 0;
    uint32_t quietUntilMs_ = 0;

    // ---- DoS: per-connection best-effort rate table ----
    struct RateEntry {
        bool     used = false;
        uint16_t conn = 0xFFFF;
        uint32_t windowStartMs = 0;
        uint8_t  offers = 0;
    };
    std::array<RateEntry, kRateTableSize> rate_{};

    // ---- Bonds to forget on disconnect (ephemeral pairing cleanup) ----
    struct BondRef {
        bool     used = false;
        uint16_t conn = 0xFFFF;
        uint8_t  addr[6] = {};
        uint8_t  addrType = 0;
    };
    std::array<BondRef, 2> bonded_{};

    // ---- Session-persistent peers (PSRAM; runtime-only trust) ----
    struct SessionPeer {
        bool    used = false;
        uint8_t addr[6] = {};
        uint8_t addrType = 0;
    };
    std::array<SessionPeer, cdc::hal::IBluetoothController::MAX_BONDED_DEVICES> sessionPeers_{};
    bool sessionBondsCleaned_ = false;  ///< Boot janitor ran once BLE was up.
    volatile bool ledgerDirty_ = false;  ///< Host-task set; tick() flushes the NVS ledger.

    // ---- Latest transfer result snapshot (read by the UI completion handler) ----
    TransferResult lastResult_{};
    bool           lastResultValid_ = false;

    // ---- Sender session ----
    struct SendSession {
        SendState state = SendState::Idle;
        uint16_t  conn = 0xFFFF;
        uint8_t   addr[6] = {};
        uint8_t   addrType = 0;
        char      mime[kMimeBufSize] = {};
        uint32_t  totalLen = 0;
        uint32_t  sent = 0;
        uint32_t  crc = 0;
        uint32_t  stateStartMs = 0;
        uint16_t  ctrlHandle = 0;
        uint16_t  statusHandle = 0;
        uint16_t  statusCccd = 0;
        uint16_t  dataHandle = 0;
        bool      peerAddrValid = false;
        uint8_t   peerAddr[6] = {};
        uint8_t   peerAddrType = 0;
        bool      completeWritten = false;
        bool      persistent = false;  ///< Remember the verified pairing this session.
        Reason    reason = Reason::None;
        // Cross-task signals consumed in tickSend().
        volatile bool connectedFlag = false;
        volatile bool discoveredFlag = false;
        volatile bool acceptedFlag = false;
        volatile bool queuedFlag = false;
        volatile bool encOk = false;
        volatile bool doneFlag = false;
        volatile bool failFlag = false;
        Reason        failReason = Reason::None;
        cdc::core::PsramUniquePtr<uint8_t> buf;
    } send_;

    bool interactiveReq_ = false;  ///< UI should open the peer picker.
};

}  // namespace cdc::msg
