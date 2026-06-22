/**
 * \file
 * \brief Generic badge-to-badge BLE message transfer service implementation.
 */

#include "cdc_msg/MessageTransfer.h"
#include "cdc_msg/MessageProfile.h"

#include "cdc_core/EventBus.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"

#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nvs.h"

#include <cstring>

namespace cdc::msg {

using cdc::core::MutexGuard;
using cdc::hal::BleUuid;
using cdc::hal::GattCharacteristic;
using cdc::hal::GattServiceDef;
using cdc::hal::IBluetoothController;
namespace GattProp = cdc::hal::GattProp;
namespace GattPerm = cdc::hal::GattPerm;

namespace {
constexpr const char* TAG = "MSG";

/// Host-task callbacks wait at most this long for the state mutex, then drop.
constexpr TickType_t kLockWaitTicks = pdMS_TO_TICKS(50);
/// Grace after notifying DONE before the receiver tears the link down.
constexpr uint32_t kDoneGraceMs = 700;
/// Throttle: notify PROGRESS at most every this many received bytes.
constexpr uint32_t kProgressStepBytes = 512;
/// Stack frame buffer for outgoing OFFER/CHUNK writes (caps the MTU-derived size).
constexpr uint16_t kFrameBufLen = 260;

/// NVS namespace + key for the session-pairing cleanup ledger (addresses only).
constexpr const char* kNsMsg     = "msg";
constexpr const char* kKeyPeers  = "speers";
/// Ledger entry size on disk: 6-byte address + 1-byte address type.
constexpr size_t kLedgerEntryLen = 7;

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

uint32_t rdU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void wrU32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t crc32(const uint8_t* data, uint32_t len)
{
    return esp_rom_crc32_le(0, data, len);
}

bool validMime(const char* mime)
{
    if (!mime) return false;
    size_t n = strnlen(mime, kMimeBufSize);
    return n > 0 && n <= kMaxMimeLen;
}
}  // namespace

MessageTransfer& MessageTransfer::instance()
{
    static MessageTransfer* inst = new MessageTransfer();
    return *inst;
}

cdc::hal::IBluetoothController* MessageTransfer::ble() const
{
    return cdc::hal::getBluetoothControllerInstance();
}

// =========================================================================
// IService lifecycle
// =========================================================================

bool MessageTransfer::init()
{
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        state_ = cdc::core::ServiceState::ERROR;
        return false;
    }
    beacon_.load();
    state_ = cdc::core::ServiceState::INITIALIZED;
    return true;
}

bool MessageTransfer::start()
{
    if (!serviceRegistered_) {
        serviceRegistered_ = registerService();
    }
    registerListeners();
    // Declare the beacon name + UUID now, before the BLE stack is brought up, so
    // the first (post-init) advertising start already carries the service UUID.
    beacon_.bootApply();
    state_ = cdc::core::ServiceState::STARTED;
    LOG_I(TAG, "Message transfer started (beacon=%s)", beacon_.isEnabled() ? "on" : "off");
    return true;
}

void MessageTransfer::stop()
{
    MutexGuard g(mutex_);
    // Abort any in-flight transfer so PSRAM buffers are freed and links closed.
    if (recv_.state != RecvState::Idle) teardownRecv(false, Reason::Disconnected);
    if (send_.state != SendState::Idle) teardownSend(false, Reason::Disconnected);
    // Session-persistent pairings never outlive the service; drop their bonds.
    forgetAllSessionPeers();
    removeListeners();
    auto* b = ble();
    if (b && serviceRegistered_) {
        b->unregisterGattService(BleUuid::from128(kMsgServiceUuid));
        serviceRegistered_ = false;
    }
    state_ = cdc::core::ServiceState::STOPPED;
}

bool MessageTransfer::registerService()
{
    auto* b = ble();
    if (!b) return false;

    static GattCharacteristic chars[3];

    // Control (plaintext WRITE): sender -> receiver, OFFER/ABORT.
    chars[0].uuid = BleUuid::from128(kMsgControlUuid);
    chars[0].properties = GattProp::WRITE;
    chars[0].permissions = GattPerm::WRITE;
    chars[0].valueHandle = nullptr;
    chars[0].onWrite = [](uint16_t c, uint16_t, const uint8_t* d, uint16_t l) -> int {
        return MessageTransfer::instance().onControlWrite(c, d, l);
    };
    chars[0].onRead = nullptr;
    chars[0].descriptors = nullptr;
    chars[0].numDescriptors = 0;

    // Status (plaintext NOTIFY): receiver -> sender.
    chars[1].uuid = BleUuid::from128(kMsgStatusUuid);
    chars[1].properties = GattProp::NOTIFY;
    chars[1].permissions = GattPerm::READ;
    chars[1].valueHandle = &statusValueHandle_;
    chars[1].onWrite = nullptr;
    chars[1].onRead = nullptr;
    chars[1].descriptors = nullptr;
    chars[1].numDescriptors = 0;

    // Data (encrypted WRITE): sender -> receiver, CHUNK/COMPLETE.
    chars[2].uuid = BleUuid::from128(kMsgDataUuid);
    chars[2].properties = GattProp::WRITE | GattProp::WRITE_NO_RSP;
    chars[2].permissions = GattPerm::WRITE_ENC;
    chars[2].valueHandle = nullptr;
    chars[2].onWrite = [](uint16_t c, uint16_t, const uint8_t* d, uint16_t l) -> int {
        return MessageTransfer::instance().onDataWrite(c, d, l);
    };
    chars[2].onRead = nullptr;
    chars[2].descriptors = nullptr;
    chars[2].numDescriptors = 0;

    static GattServiceDef svc;
    svc.uuid = BleUuid::from128(kMsgServiceUuid);
    svc.characteristics = chars;
    svc.numCharacteristics = 3;

    if (!b->registerGattService(svc, false)) {
        LOG_E(TAG, "Failed to register message GATT service");
        return false;
    }
    return true;
}

void MessageTransfer::registerListeners()
{
    auto* b = ble();
    if (!b) return;
    tokConn_ = b->addConnectionCallback(
        [](uint16_t c) { MessageTransfer::instance().onConnect(c); });
    tokDisconn_ = b->addDisconnectionCallback(
        [](uint16_t c, int r) { MessageTransfer::instance().onDisconnect(c, r); });
    tokSvcDisc_ = b->addServiceDiscoveryCallback(
        [](uint16_t c, const IBluetoothController::DiscoveredService* s, bool comp) {
            MessageTransfer::instance().onServiceDiscovered(c, s, comp);
        });
    tokNotify_ = b->addNotificationCallback(
        [](uint16_t c, uint16_t a, const uint8_t* d, uint16_t l) {
            MessageTransfer::instance().onNotification(c, a, d, l);
        });
    tokWrite_ = b->addWriteCompleteCallback(
        [](uint16_t c, uint16_t a, int s) { MessageTransfer::instance().onWriteComplete(c, a, s); });
    tokEnc_ = b->addEncryptionChangeCallback(
        [](uint16_t c, int s) { MessageTransfer::instance().onEncChange(c, s); });
}

void MessageTransfer::removeListeners()
{
    auto* b = ble();
    if (!b) return;
    b->removeConnectionCallback(tokConn_);
    b->removeDisconnectionCallback(tokDisconn_);
    b->removeServiceDiscoveryCallback(tokSvcDisc_);
    b->removeNotificationCallback(tokNotify_);
    b->removeWriteCompleteCallback(tokWrite_);
    b->removeEncryptionChangeCallback(tokEnc_);
    tokConn_ = tokDisconn_ = tokSvcDisc_ = tokNotify_ = tokWrite_ = tokEnc_ =
        cdc::hal::INVALID_LISTENER;
}

// =========================================================================
// Handler registry
// =========================================================================

bool MessageTransfer::registerHandler(const char* mime, const char* descKey, DeliverFn deliver)
{
    MutexGuard g(mutex_);
    return registry_.registerHandler(mime, descKey, std::move(deliver));
}

void MessageTransfer::unregisterHandler(const char* mime)
{
    MutexGuard g(mutex_);
    registry_.unregisterHandler(mime);
}

bool MessageTransfer::hasHandler(const char* mime)
{
    MutexGuard g(mutex_);
    return anyHandler(mime);
}

bool MessageTransfer::anyHandler(const char* mime)
{
    if (registry_.hasHandler(mime)) return true;
    return canHandle_ && canHandle_(mime);
}

void MessageTransfer::setDeferredHandler(CanHandleFn canHandle, DeferredDeliverFn deliver)
{
    MutexGuard g(mutex_);
    canHandle_ = std::move(canHandle);
    deferredDeliver_ = std::move(deliver);
}

// =========================================================================
// Beacon
// =========================================================================

void MessageTransfer::setBeaconEnabled(bool enabled)
{
    MutexGuard g(mutex_);
    beacon_.setEnabled(enabled);
}

// =========================================================================
// Send (central)
// =========================================================================

bool MessageTransfer::sendTo(const uint8_t addr[6], uint8_t addrType, const char* mime,
                             const uint8_t* data, uint32_t len, bool persistent)
{
    if (!addr || !data || !validMime(mime) || len == 0 || len > kMaxPayloadBytes) return false;

    MutexGuard g(mutex_);
    if (send_.state != SendState::Idle) return false;
    auto* b = ble();
    if (!b || !b->isEnabled()) return false;

    send_.buf = cdc::core::psramAlloc<uint8_t>(len);
    if (!send_.buf) return false;
    std::memcpy(send_.buf.get(), data, len);
    send_.totalLen = len;
    send_.sent = 0;
    send_.crc = crc32(send_.buf.get(), len);
    std::strncpy(send_.mime, mime, sizeof(send_.mime) - 1);
    send_.mime[sizeof(send_.mime) - 1] = '\0';
    std::memcpy(send_.addr, addr, 6);
    send_.addrType = addrType;
    send_.persistent = persistent;
    send_.completeWritten = false;
    send_.peerAddrValid = false;
    send_.connectedFlag = send_.discoveredFlag = send_.acceptedFlag = false;
    send_.queuedFlag = send_.encOk = send_.doneFlag = send_.failFlag = false;
    send_.conn = 0xFFFF;
    send_.state = SendState::Connecting;
    send_.stateStartMs = nowMs();

    if (!b->connect(addr, addrType)) {
        teardownSend(false, Reason::Disconnected);
        return false;
    }
    return true;
}

bool MessageTransfer::beginInteractiveSend(const char* mime, const uint8_t* data, uint32_t len,
                                           bool persistent)
{
    if (!data || !validMime(mime) || len == 0 || len > kMaxPayloadBytes) return false;

    MutexGuard g(mutex_);
    if (send_.state != SendState::Idle) return false;
    auto* b = ble();
    if (!b || !b->isEnabled()) return false;

    send_.buf = cdc::core::psramAlloc<uint8_t>(len);
    if (!send_.buf) return false;
    std::memcpy(send_.buf.get(), data, len);
    send_.totalLen = len;
    send_.sent = 0;
    send_.crc = crc32(send_.buf.get(), len);
    std::strncpy(send_.mime, mime, sizeof(send_.mime) - 1);
    send_.mime[sizeof(send_.mime) - 1] = '\0';
    send_.persistent = persistent;
    send_.completeWritten = false;
    send_.peerAddrValid = false;
    send_.conn = 0xFFFF;
    send_.state = SendState::PickingPeer;
    send_.stateStartMs = nowMs();
    interactiveReq_ = true;

    b->startScan(6000);
    return true;
}

bool MessageTransfer::takeInteractiveRequest()
{
    MutexGuard g(mutex_);
    if (!interactiveReq_) return false;
    interactiveReq_ = false;
    return true;
}

bool MessageTransfer::confirmInteractiveTarget(const uint8_t addr[6], uint8_t addrType)
{
    if (!addr) return false;
    MutexGuard g(mutex_);
    if (send_.state != SendState::PickingPeer) return false;
    auto* b = ble();
    if (!b) {
        teardownSend(false, Reason::Disconnected);
        return false;
    }
    b->stopScan();
    std::memcpy(send_.addr, addr, 6);
    send_.addrType = addrType;
    send_.sent = 0;
    send_.completeWritten = false;
    send_.peerAddrValid = false;
    send_.connectedFlag = send_.discoveredFlag = send_.acceptedFlag = false;
    send_.queuedFlag = send_.encOk = send_.doneFlag = send_.failFlag = false;
    send_.conn = 0xFFFF;
    send_.state = SendState::Connecting;
    send_.stateStartMs = nowMs();

    if (!b->connect(addr, addrType)) {
        teardownSend(false, Reason::Disconnected);
        return false;
    }
    return true;
}

void MessageTransfer::cancelSend()
{
    MutexGuard g(mutex_);
    if (send_.state == SendState::Idle) return;
    teardownSend(false, Reason::UserDeclined);
}

uint32_t MessageTransfer::sendProgress(uint32_t* outTotal) const
{
    if (outTotal) *outTotal = send_.totalLen;
    return send_.sent;
}

// =========================================================================
// Discovery
// =========================================================================

bool MessageTransfer::startDiscovery(uint32_t durationMs, bool keepAdvertising)
{
    auto* b = ble();
    if (!b || !b->isEnabled()) return false;
    return b->startScan(durationMs, keepAdvertising);
}

void MessageTransfer::stopDiscovery()
{
    auto* b = ble();
    if (b) b->stopScan();
}

bool MessageTransfer::discoveryDone() const
{
    auto* b = ble();
    return !b || b->isScanComplete();
}

uint8_t MessageTransfer::getPeers(PeerInfo* out, uint8_t maxPeers)
{
    if (!out || maxPeers == 0) return 0;
    auto* b = ble();
    if (!b) return 0;

    static cdc::hal::BleScanResult results[cdc::hal::IBluetoothController::MAX_SCAN_RESULTS];
    uint8_t n = b->getScanResults(results, cdc::hal::IBluetoothController::MAX_SCAN_RESULTS);

    uint8_t count = 0;
    for (uint8_t i = 0; i < n && count < maxPeers; ++i) {
        if (!cdc::hal::BleAdvParser::findServiceUuid128(
                results[i].advData, results[i].advDataLen, kMsgServiceUuid)) {
            continue;
        }
        PeerInfo& p = out[count];
        std::strncpy(p.name, results[i].name, sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = '\0';
        std::memcpy(p.addr, results[i].mac, 6);
        p.addrType = results[i].addrType;
        p.rssi = results[i].rssi;
        ++count;
    }
    return count;
}

// =========================================================================
// Receiver consent
// =========================================================================

bool MessageTransfer::hasPendingConsent() const
{
    return recv_.state == RecvState::AwaitingConsent;
}

bool MessageTransfer::getPendingConsent(char* peerName, size_t nameSize, char* mime,
                                        size_t mimeSize, const char** outDescKey,
                                        uint32_t* outSize) const
{
    MutexGuard g(const_cast<MessageTransfer*>(this)->mutex_);
    if (recv_.state != RecvState::AwaitingConsent) return false;
    if (peerName && nameSize) {
        std::strncpy(peerName, recv_.peerName, nameSize - 1);
        peerName[nameSize - 1] = '\0';
    }
    if (mime && mimeSize) {
        std::strncpy(mime, recv_.mime, mimeSize - 1);
        mime[mimeSize - 1] = '\0';
    }
    if (outDescKey) *outDescKey = recv_.descKey;
    if (outSize) *outSize = recv_.totalLen;
    return true;
}

void MessageTransfer::respondConsent(bool accept)
{
    MutexGuard g(mutex_);
    if (recv_.state != RecvState::AwaitingConsent) return;

    // The peer may have dropped while the prompt was up; don't proceed to a
    // 30 s encryption wait on a dead handle.
    if (recv_.failFlag) {
        teardownRecv(false, Reason::None);
        return;
    }

    if (accept) {
        notifyStatus(recv_.conn, StatusOp::Accept);
        recv_.state = RecvState::AwaitingEncrypt;
        recv_.stateStartMs = nowMs();
    } else {
        notifyStatusByte(recv_.conn, StatusOp::Decline, static_cast<uint8_t>(Reason::UserDeclined));
        quietUntilMs_ = nowMs() + kQuietCooldownMs;
        teardownRecv(false, Reason::None);
    }
}

uint32_t MessageTransfer::recvProgress(uint32_t* outTotal) const
{
    if (outTotal) *outTotal = recv_.totalLen;
    return recv_.received;
}

bool MessageTransfer::consumeResult(TransferResult* out)
{
    MutexGuard g(mutex_);
    if (!lastResultValid_) return false;
    if (out) *out = lastResult_;
    lastResultValid_ = false;
    return true;
}

// =========================================================================
// GATT server write callbacks (host task)
// =========================================================================

int MessageTransfer::onControlWrite(uint16_t conn, const uint8_t* data, uint16_t len)
{
    if (!data || len < 1) return 0;
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return 0;

    const uint8_t op = data[0];

    if (op == static_cast<uint8_t>(ControlOp::Abort)) {
        if (recv_.state != RecvState::Idle && conn == recv_.conn) {
            recv_.failFlag = true;
            recv_.failReason = Reason::Disconnected;
        }
        return 0;
    }
    if (op != static_cast<uint8_t>(ControlOp::Offer)) return 0;

    // --- Bounds-first OFFER validation (never trust the wire) ---
    if (len < kOfferHeaderLen) {
        notifyStatusByte(conn, StatusOp::Decline, static_cast<uint8_t>(Reason::BadFrame));
        return 0;
    }
    const uint8_t  ver = data[1];
    const uint32_t totalLen = rdU32(&data[2]);
    const uint8_t  mimeLen = data[6];
    const uint8_t  nameByte = data[7];
    const uint8_t  nameLen = nameByte & kOfferNameLenMask;
    const bool     persistReq = (nameByte & kOfferFlagPersist) != 0;

    if (ver != kProtocolVersion || mimeLen == 0 || mimeLen > kMaxMimeLen ||
        nameLen > kMaxNameLen ||
        static_cast<uint32_t>(kOfferHeaderLen) + mimeLen + nameLen > len) {
        notifyStatusByte(conn, StatusOp::Decline, static_cast<uint8_t>(Reason::BadFrame));
        return 0;
    }
    if (totalLen == 0 || totalLen > kMaxPayloadBytes) {
        notifyStatusByte(conn, StatusOp::Decline, static_cast<uint8_t>(Reason::TooLarge));
        return 0;
    }

    char mime[kMimeBufSize] = {};
    std::memcpy(mime, &data[kOfferHeaderLen], mimeLen);
    mime[mimeLen] = '\0';
    char name[kNameBufSize] = {};
    if (nameLen > 0) {
        std::memcpy(name, &data[kOfferHeaderLen + mimeLen], nameLen);
        name[nameLen] = '\0';
    }

    if (!anyHandler(mime)) {
        notifyStatusByte(conn, StatusOp::Decline, static_cast<uint8_t>(Reason::NoHandler));
        return 0;
    }

    const uint32_t now = nowMs();
    if (!rateLimitOk(conn, now)) {
        notifyStatus(conn, StatusOp::Busy);
        return 0;
    }

    enqueueOffer(conn, mime, totalLen, name, persistReq);
    return 0;
}

int MessageTransfer::onDataWrite(uint16_t conn, const uint8_t* data, uint16_t len)
{
    if (!data || len < 1) return 0;
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return 0;

    if (recv_.state != RecvState::Receiving || conn != recv_.conn) return 0;

    const uint8_t op = data[0];

    if (op == static_cast<uint8_t>(DataOp::Chunk)) {
        if (len < kChunkHeaderLen) {
            recv_.failFlag = true;
            recv_.failReason = Reason::BadFrame;
            return 0;
        }
        const uint32_t payloadLen = static_cast<uint32_t>(len) - kChunkHeaderLen;
        // Subtraction-form bounds guard (no wrap): received <= total, payload <= remaining.
        if (recv_.received > recv_.totalLen ||
            payloadLen > recv_.totalLen - recv_.received) {
            recv_.failFlag = true;
            recv_.failReason = Reason::BadFrame;
            return 0;
        }
        if (recv_.buf) {
            std::memcpy(recv_.buf.get() + recv_.received, &data[kChunkHeaderLen], payloadLen);
        }
        recv_.received += payloadLen;
        recv_.stateStartMs = nowMs();
        if (recv_.received - recv_.lastProgress >= kProgressStepBytes ||
            recv_.received == recv_.totalLen) {
            recv_.lastProgress = recv_.received;
            notifyStatusU32(conn, StatusOp::Progress, recv_.received);
        }
    } else if (op == static_cast<uint8_t>(DataOp::Complete)) {
        if (len < kCompleteLen) {
            recv_.failFlag = true;
            recv_.failReason = Reason::BadFrame;
            return 0;
        }
        if (recv_.received != recv_.totalLen) {
            recv_.failFlag = true;
            recv_.failReason = Reason::CrcMismatch;
            return 0;
        }
        recv_.expectedCrc = rdU32(&data[2]);
        recv_.completeReceived = true;  // tick verifies CRC + delivers
    }
    return 0;
}

// =========================================================================
// Shared BLE callbacks (host task)
// =========================================================================

void MessageTransfer::onConnect(uint16_t conn)
{
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;
    if (send_.state == SendState::Connecting && send_.conn == 0xFFFF) {
        send_.conn = conn;
        send_.connectedFlag = true;
    }
}

void MessageTransfer::onDisconnect(uint16_t conn, int reason)
{
    (void)reason;
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;
    forgetBondForConn(conn);
    if (send_.state != SendState::Idle && conn == send_.conn) {
        send_.failFlag = true;
        send_.failReason = Reason::Disconnected;
    }
    if (recv_.state != RecvState::Idle && conn == recv_.conn) {
        if (recv_.state == RecvState::Done) {
            resetRecv();  // graceful close after a successful transfer
        } else {
            recv_.failFlag = true;
            recv_.failReason = Reason::Disconnected;
        }
    }
    dropQueuedForConn(conn);
}

void MessageTransfer::onServiceDiscovered(
    uint16_t conn, const IBluetoothController::DiscoveredService* svc, bool complete)
{
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;
    if (send_.state != SendState::Discovering || conn != send_.conn || !complete) return;
    if (!svc) {
        send_.failFlag = true;
        send_.failReason = Reason::BadFrame;
        return;
    }

    const BleUuid ctrl = BleUuid::from128(kMsgControlUuid);
    const BleUuid stat = BleUuid::from128(kMsgStatusUuid);
    const BleUuid data = BleUuid::from128(kMsgDataUuid);

    send_.ctrlHandle = send_.statusHandle = send_.dataHandle = send_.statusCccd = 0;
    for (uint8_t i = 0; i < svc->numCharacteristics; ++i) {
        const auto& chr = svc->characteristics[i];
        if (chr.uuid == ctrl) {
            send_.ctrlHandle = chr.valueHandle;
        } else if (chr.uuid == stat) {
            send_.statusHandle = chr.valueHandle;
            send_.statusCccd = chr.valueHandle + 1;
        } else if (chr.uuid == data) {
            send_.dataHandle = chr.valueHandle;
        }
    }
    if (send_.ctrlHandle == 0 || send_.dataHandle == 0 || send_.statusHandle == 0) {
        send_.failFlag = true;
        send_.failReason = Reason::BadFrame;
        return;
    }
    send_.discoveredFlag = true;
}

void MessageTransfer::onNotification(uint16_t conn, uint16_t attr, const uint8_t* data, uint16_t len)
{
    (void)attr;
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;
    if (send_.state == SendState::Idle || conn != send_.conn || !data || len < 1) return;

    switch (static_cast<StatusOp>(data[0])) {
        case StatusOp::Accept:
            send_.acceptedFlag = true;
            break;
        case StatusOp::Decline:
            send_.failFlag = true;
            send_.failReason = (len >= 2) ? static_cast<Reason>(data[1]) : Reason::UserDeclined;
            break;
        case StatusOp::Busy:
            send_.failFlag = true;
            send_.failReason = Reason::Busy;
            break;
        case StatusOp::Queued:
            send_.queuedFlag = true;
            break;
        case StatusOp::Error:
            send_.failFlag = true;
            send_.failReason = (len >= 2) ? static_cast<Reason>(data[1]) : Reason::None;
            break;
        case StatusOp::Done:
            send_.doneFlag = true;
            break;
        default:
            break;
    }
}

void MessageTransfer::onWriteComplete(uint16_t conn, uint16_t attr, int status)
{
    (void)attr;
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;
    if (send_.state == SendState::Idle || conn != send_.conn) return;
    if (status != 0) {
        send_.failFlag = true;
        send_.failReason = Reason::Disconnected;
    }
}

void MessageTransfer::onEncChange(uint16_t conn, int status)
{
    auto g = MutexGuard::tryLock(mutex_, kLockWaitTicks);
    if (!g.locked()) return;

    // Sender side.
    if (send_.state == SendState::AwaitingEncrypt && conn == send_.conn) {
        if (status == 0) {
            uint8_t a[6]; uint8_t t = 0;
            if (ble() && ble()->getPeerIdAddr(conn, a, &t)) {
                std::memcpy(send_.peerAddr, a, 6);
                send_.peerAddrType = t;
                send_.peerAddrValid = true;
                recordBond(conn, a, t);
                if (send_.persistent) rememberSessionPeer(a, t);
            }
            send_.encOk = true;
        } else {
            send_.failFlag = true;
            send_.failReason = Reason::PairFailed;
        }
        return;
    }

    // Receiver side: allocate the reassembly buffer before chunks arrive.
    if (recv_.state == RecvState::AwaitingEncrypt && conn == recv_.conn) {
        if (status == 0) {
            recv_.buf = cdc::core::psramAlloc<uint8_t>(recv_.totalLen);
            if (!recv_.buf) {
                recv_.failFlag = true;
                recv_.failReason = Reason::TooLarge;
                return;
            }
            recv_.received = 0;
            recv_.lastProgress = 0;
            recv_.completeReceived = false;
            uint8_t a[6]; uint8_t t = 0;
            if (ble() && ble()->getPeerIdAddr(conn, a, &t)) {
                std::memcpy(recv_.peerAddr, a, 6);
                recv_.peerAddrType = t;
                recv_.peerAddrValid = true;
                recordBond(conn, a, t);
                if (recv_.persistent) rememberSessionPeer(a, t);
            }
            recv_.state = RecvState::Receiving;
            recv_.stateStartMs = nowMs();
        } else {
            recv_.failFlag = true;
            recv_.failReason = Reason::PairFailed;
        }
    }
}

// =========================================================================
// Status notifications
// =========================================================================

void MessageTransfer::notifyStatus(uint16_t conn, StatusOp op)
{
    auto* b = ble();
    if (!b || conn == 0xFFFF || statusValueHandle_ == 0) return;
    uint8_t buf[1] = {static_cast<uint8_t>(op)};
    b->sendNotification(conn, statusValueHandle_, buf, sizeof(buf));
}

void MessageTransfer::notifyStatusByte(uint16_t conn, StatusOp op, uint8_t arg)
{
    auto* b = ble();
    if (!b || conn == 0xFFFF || statusValueHandle_ == 0) return;
    uint8_t buf[2] = {static_cast<uint8_t>(op), arg};
    b->sendNotification(conn, statusValueHandle_, buf, sizeof(buf));
}

void MessageTransfer::notifyStatusU32(uint16_t conn, StatusOp op, uint32_t value)
{
    auto* b = ble();
    if (!b || conn == 0xFFFF || statusValueHandle_ == 0) return;
    uint8_t buf[5];
    buf[0] = static_cast<uint8_t>(op);
    wrU32(&buf[1], value);
    b->sendNotification(conn, statusValueHandle_, buf, sizeof(buf));
}

// =========================================================================
// Receiver helpers
// =========================================================================

void MessageTransfer::enqueueOffer(uint16_t conn, const char* mime, uint32_t totalLen,
                                   const char* name, bool persist)
{
    int slot = -1;
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (!queue_[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        notifyStatus(conn, StatusOp::Busy);  // queue full -> graceful saturation
        return;
    }
    PendingOffer& q = queue_[static_cast<size_t>(slot)];
    q.used = true;
    q.conn = conn;
    std::strncpy(q.mime, mime, sizeof(q.mime) - 1);
    q.mime[sizeof(q.mime) - 1] = '\0';
    std::strncpy(q.peerName, name, sizeof(q.peerName) - 1);
    q.peerName[sizeof(q.peerName) - 1] = '\0';
    q.descKey = registry_.descKey(mime);
    q.totalLen = totalLen;
    q.persist = persist;
    q.enqueuedMs = nowMs();

    if (recv_.state != RecvState::Idle) {
        notifyStatus(conn, StatusOp::Queued);
    }
}

void MessageTransfer::dropQueuedForConn(uint16_t conn)
{
    for (auto& q : queue_) {
        if (q.used && q.conn == conn) q.used = false;
    }
}

bool MessageTransfer::tryConsumePromptBudget(uint32_t now)
{
    if (now - promptWindowStartMs_ > kPromptWindowMs) {
        promptWindowStartMs_ = now;
        promptsInWindow_ = 0;
    }
    if (promptsInWindow_ >= kMaxPromptsPerWindow) return false;
    ++promptsInWindow_;
    return true;
}

bool MessageTransfer::rateLimitOk(uint16_t conn, uint32_t now)
{
    RateEntry* e = nullptr;
    for (auto& r : rate_) {
        if (r.used && r.conn == conn) {
            e = &r;
            break;
        }
    }
    if (!e) {
        for (auto& r : rate_) {
            if (!r.used) {
                e = &r;
                e->used = true;
                e->conn = conn;
                e->windowStartMs = now;
                e->offers = 0;
                break;
            }
        }
    }
    if (!e) return true;  // table full: do not over-restrict (global budget still applies)
    if (now - e->windowStartMs > kOfferWindowMs) {
        e->windowStartMs = now;
        e->offers = 0;
    }
    if (e->offers >= kMaxOffersPerWindow) return false;
    ++e->offers;
    return true;
}

void MessageTransfer::promoteNextOffer(uint32_t now)
{
    if (recv_.state != RecvState::Idle) return;

    int head = -1;
    uint32_t oldest = 0;
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (!queue_[i].used) continue;
        if (head < 0 || queue_[i].enqueuedMs < oldest) {
            oldest = queue_[i].enqueuedMs;
            head = static_cast<int>(i);
        }
    }
    if (head < 0) return;

    PendingOffer& q = queue_[static_cast<size_t>(head)];

    if (now - q.enqueuedMs > kQueueWaitTimeoutMs) {
        notifyStatusByte(q.conn, StatusOp::Error, static_cast<uint8_t>(Reason::Timeout));
        q.used = false;
        return;
    }

    // A persistent offer from a peer already trusted this session skips the
    // consent prompt (and the global prompt budget): the pairing was verified
    // once and the bond is reused, so there is no new human decision to make.
    bool autoAccept = false;
    if (q.persist && ble()) {
        uint8_t a[6]; uint8_t t = 0;
        if (ble()->getPeerIdAddr(q.conn, a, &t) && isSessionPeer(a, t)) autoAccept = true;
    }

    if (!autoAccept && (now < quietUntilMs_ || !tryConsumePromptBudget(now))) {
        notifyStatus(q.conn, StatusOp::Busy);  // address-independent throttle
        q.used = false;
        return;
    }

    recv_.conn = q.conn;
    std::strncpy(recv_.mime, q.mime, sizeof(recv_.mime) - 1);
    recv_.mime[sizeof(recv_.mime) - 1] = '\0';
    std::strncpy(recv_.peerName, q.peerName, sizeof(recv_.peerName) - 1);
    recv_.peerName[sizeof(recv_.peerName) - 1] = '\0';
    recv_.descKey = q.descKey;
    recv_.totalLen = q.totalLen;
    recv_.persistent = q.persist;
    recv_.received = 0;
    recv_.lastProgress = 0;
    recv_.completeReceived = false;
    recv_.peerAddrValid = false;
    recv_.encOk = false;
    recv_.failFlag = false;
    recv_.failReason = Reason::None;
    recv_.stateStartMs = now;
    q.used = false;

    if (autoAccept) {
        notifyStatus(recv_.conn, StatusOp::Accept);
        recv_.state = RecvState::AwaitingEncrypt;
    } else {
        recv_.state = RecvState::AwaitingConsent;
        cdc::core::EventBus::instance().publish(cdc::core::EventType::BLE_CONSENT_REQUEST);
    }
}

void MessageTransfer::tickRecv(uint32_t now)
{
    switch (recv_.state) {
        case RecvState::Idle:
            promoteNextOffer(now);
            break;

        case RecvState::AwaitingConsent:
            if (recv_.failFlag) {
                teardownRecv(false, recv_.failReason);
            } else if (now - recv_.stateStartMs > kConsentTimeoutMs) {
                teardownRecv(true, Reason::Timeout);
            }
            break;

        case RecvState::AwaitingEncrypt:
            if (recv_.failFlag) {
                teardownRecv(true, recv_.failReason);
            } else if (now - recv_.stateStartMs > kEncryptTimeoutMs) {
                teardownRecv(true, Reason::Timeout);
            }
            break;

        case RecvState::Receiving:
            if (recv_.failFlag) {
                teardownRecv(true, recv_.failReason);
            } else if (recv_.completeReceived) {
                if (crc32(recv_.buf.get(), recv_.totalLen) != recv_.expectedCrc) {
                    teardownRecv(true, Reason::CrcMismatch);
                    break;
                }
                bool ok;
                if (registry_.hasHandler(recv_.mime)) {
                    ok = registry_.deliver(recv_.mime, recv_.buf.get(), recv_.totalLen,
                                           recv_.peerName);
                } else if (deferredDeliver_) {
                    // Handled by a not-yet-loaded plugin: hand off for async
                    // load + dispatch on the plugin task.
                    ok = deferredDeliver_(recv_.buf.get(), recv_.totalLen, recv_.mime,
                                          recv_.peerName);
                } else {
                    ok = false;
                }
                if (ok) {
                    notifyStatus(recv_.conn, StatusOp::Done);
                    storeResult(true, false, Reason::None, recv_.mime, recv_.peerName);
                    cdc::core::EventBus::instance().publish(
                        cdc::core::EventType::BLE_EXCHANGE_COMPLETE);
                    recv_.buf.reset();
                    recv_.state = RecvState::Done;  // grace before disconnect (let DONE flush)
                    recv_.stateStartMs = now;
                } else {
                    teardownRecv(true, Reason::BadFrame);
                }
            } else if (now - recv_.stateStartMs > kTransferIdleTimeoutMs) {
                teardownRecv(true, Reason::Timeout);
            }
            break;

        case RecvState::Done:
            if (now - recv_.stateStartMs > kDoneGraceMs) {
                uint16_t conn = recv_.conn;
                resetRecv();
                if (conn != 0xFFFF && ble()) ble()->disconnectHandle(conn);
            }
            break;

        default:
            break;
    }
}

void MessageTransfer::teardownRecv(bool sendError, Reason reason)
{
    uint16_t conn = recv_.conn;
    if (sendError && conn != 0xFFFF) {
        notifyStatusByte(conn, StatusOp::Error, static_cast<uint8_t>(reason));
        storeResult(false, false, reason, recv_.mime, recv_.peerName);
        cdc::core::EventBus::instance().publish(cdc::core::EventType::BLE_EXCHANGE_COMPLETE);
    }
    resetRecv();
    if (conn != 0xFFFF && ble()) ble()->disconnectHandle(conn);
}

void MessageTransfer::resetRecv()
{
    recv_.state = RecvState::Idle;
    recv_.conn = 0xFFFF;
    recv_.mime[0] = '\0';
    recv_.peerName[0] = '\0';
    recv_.descKey = nullptr;
    recv_.totalLen = 0;
    recv_.received = 0;
    recv_.lastProgress = 0;
    recv_.expectedCrc = 0;
    recv_.completeReceived = false;
    recv_.persistent = false;
    recv_.peerAddrValid = false;
    recv_.encOk = false;
    recv_.failFlag = false;
    recv_.failReason = Reason::None;
    recv_.buf.reset();
}

// =========================================================================
// Sender helpers
// =========================================================================

uint16_t MessageTransfer::mtuPayload() const
{
    auto* b = ble();
    uint16_t mtu = b ? b->getMtu() : 20;
    return mtu < 20 ? 20 : mtu;
}

void MessageTransfer::streamChunks()
{
    auto* b = ble();
    if (!b || !send_.buf) return;

    uint8_t frame[kFrameBufLen];
    const uint16_t mtu = mtuPayload();
    if (mtu <= kChunkHeaderLen) return;
    // Clamp to the stack frame buffer: never trust the negotiated MTU to stay
    // within sizeof(frame) (a larger CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU must not
    // overflow this buffer).
    uint16_t maxChunk = mtu - kChunkHeaderLen;
    if (maxChunk > sizeof(frame) - kChunkHeaderLen) {
        maxChunk = sizeof(frame) - kChunkHeaderLen;
    }
    while (send_.sent < send_.totalLen) {
        const uint32_t remaining = send_.totalLen - send_.sent;
        const uint16_t chunk =
            static_cast<uint16_t>(remaining > maxChunk ? maxChunk : remaining);
        frame[0] = static_cast<uint8_t>(DataOp::Chunk);
        frame[1] = 0;
        std::memcpy(&frame[kChunkHeaderLen], send_.buf.get() + send_.sent, chunk);
        if (!b->writeCharacteristic(send_.conn, send_.dataHandle, frame,
                                    kChunkHeaderLen + chunk, false)) {
            return;  // congested; resume next tick
        }
        send_.sent += chunk;
        send_.stateStartMs = nowMs();
    }

    if (!send_.completeWritten) {
        uint8_t comp[kCompleteLen];
        comp[0] = static_cast<uint8_t>(DataOp::Complete);
        comp[1] = 0;
        wrU32(&comp[2], send_.crc);
        if (b->writeCharacteristic(send_.conn, send_.dataHandle, comp, kCompleteLen, true)) {
            send_.completeWritten = true;
            send_.stateStartMs = nowMs();
        }
    }
}

void MessageTransfer::tickSend(uint32_t now)
{
    switch (send_.state) {
        case SendState::Idle:
            break;

        case SendState::PickingPeer:
            // The UI owns the picker; bail out if it never resolves so the
            // buffered payload + busy state don't wedge future sends.
            if (now - send_.stateStartMs > kQueueWaitTimeoutMs) {
                teardownSend(false, Reason::Timeout);
            }
            break;

        case SendState::Connecting:
            if (send_.failFlag) {
                teardownSend(false, send_.failReason);
            } else if (send_.connectedFlag) {
                send_.connectedFlag = false;
                auto* b = ble();
                if (b && b->discoverServiceByUuid(send_.conn, BleUuid::from128(kMsgServiceUuid))) {
                    send_.state = SendState::Discovering;
                    send_.stateStartMs = now;
                } else {
                    teardownSend(false, Reason::Disconnected);
                }
            } else if (now - send_.stateStartMs > kConnectTimeoutMs) {
                teardownSend(false, Reason::Timeout);
            }
            break;

        case SendState::Discovering:
            if (send_.failFlag) {
                teardownSend(false, send_.failReason);
            } else if (send_.discoveredFlag) {
                send_.discoveredFlag = false;
                auto* b = ble();
                if (!b) {
                    teardownSend(false, Reason::Disconnected);
                    break;
                }
                b->enableNotifications(send_.conn, send_.statusCccd);

                const uint8_t mimeLen =
                    static_cast<uint8_t>(strnlen(send_.mime, kMaxMimeLen));
                const char* bname = beacon_.getName();
                uint8_t nameLen = static_cast<uint8_t>(strnlen(bname, kMaxNameLen));
                // Bound the OFFER to both the negotiated MTU and the frame buffer.
                uint16_t mtu = mtuPayload();
                if (mtu > kFrameBufLen) mtu = kFrameBufLen;
                uint16_t need = kOfferHeaderLen + mimeLen + nameLen;
                if (need > mtu) {
                    if (static_cast<uint16_t>(kOfferHeaderLen + mimeLen) > mtu) {
                        teardownSend(false, Reason::BadFrame);
                        break;
                    }
                    nameLen = static_cast<uint8_t>(mtu - kOfferHeaderLen - mimeLen);
                    need = kOfferHeaderLen + mimeLen + nameLen;
                }
                uint8_t frame[kFrameBufLen];
                frame[0] = static_cast<uint8_t>(ControlOp::Offer);
                frame[1] = kProtocolVersion;
                wrU32(&frame[2], send_.totalLen);
                frame[6] = mimeLen;
                frame[7] = nameLen | (send_.persistent ? kOfferFlagPersist : 0);
                std::memcpy(&frame[kOfferHeaderLen], send_.mime, mimeLen);
                if (nameLen > 0) {
                    std::memcpy(&frame[kOfferHeaderLen + mimeLen], bname, nameLen);
                }
                if (b->writeCharacteristic(send_.conn, send_.ctrlHandle, frame, need, true)) {
                    send_.state = SendState::Offering;
                    send_.stateStartMs = now;
                } else {
                    teardownSend(false, Reason::Disconnected);
                }
            } else if (now - send_.stateStartMs > kDiscoveryTimeoutMs) {
                teardownSend(false, Reason::Timeout);
            }
            break;

        case SendState::Offering:
            if (send_.failFlag) {
                teardownSend(false, send_.failReason);
            } else if (send_.acceptedFlag) {
                send_.acceptedFlag = false;
                auto* b = ble();
                if (b && b->initiateSecurity(send_.conn)) {
                    send_.state = SendState::AwaitingEncrypt;
                    send_.stateStartMs = now;
                } else {
                    teardownSend(false, Reason::PairFailed);
                }
            } else {
                if (send_.queuedFlag) {
                    send_.queuedFlag = false;
                    send_.stateStartMs = now;  // extend wait while queued
                }
                if (now - send_.stateStartMs > kQueueWaitTimeoutMs) {
                    teardownSend(false, Reason::Timeout);
                }
            }
            break;

        case SendState::AwaitingEncrypt:
            if (send_.failFlag) {
                teardownSend(false, send_.failReason);
            } else if (send_.encOk) {
                send_.encOk = false;
                send_.state = SendState::Streaming;
                send_.stateStartMs = now;
                streamChunks();
            } else if (now - send_.stateStartMs > kEncryptTimeoutMs) {
                teardownSend(false, Reason::Timeout);
            }
            break;

        case SendState::Streaming:
            if (send_.failFlag) {
                teardownSend(false, send_.failReason);
            } else if (send_.doneFlag) {
                teardownSend(true, Reason::None);
            } else {
                if (send_.sent < send_.totalLen || !send_.completeWritten) {
                    streamChunks();
                }
                // stateStartMs advances on each successful chunk/COMPLETE write,
                // so this also catches a mid-stream congestion stall (no guard on
                // completeWritten, else a stuck stream would never time out).
                if (now - send_.stateStartMs > kTransferIdleTimeoutMs) {
                    teardownSend(false, Reason::Timeout);
                }
            }
            break;

        default:
            break;
    }
}

void MessageTransfer::teardownSend(bool ok, Reason reason)
{
    storeResult(ok, true, reason, send_.mime, "");
    uint16_t conn = send_.conn;
    resetSend();
    auto* b = ble();
    if (b) {
        if (conn != 0xFFFF) {
            b->disconnectHandle(conn);  // onDisconnect forgets the bond
        } else {
            b->cancelConnect();
        }
    }
    cdc::core::EventBus::instance().publish(cdc::core::EventType::BLE_EXCHANGE_COMPLETE);
}

void MessageTransfer::resetSend()
{
    send_.state = SendState::Idle;
    send_.conn = 0xFFFF;
    send_.mime[0] = '\0';
    send_.totalLen = 0;
    send_.sent = 0;
    send_.crc = 0;
    send_.ctrlHandle = send_.statusHandle = send_.statusCccd = send_.dataHandle = 0;
    send_.peerAddrValid = false;
    send_.completeWritten = false;
    send_.persistent = false;
    send_.reason = Reason::None;
    send_.connectedFlag = send_.discoveredFlag = send_.acceptedFlag = false;
    send_.queuedFlag = send_.encOk = send_.doneFlag = send_.failFlag = false;
    send_.failReason = Reason::None;
    send_.buf.reset();
    interactiveReq_ = false;
}

// =========================================================================
// Bond tracking (forget ephemeral pairings on disconnect)
// =========================================================================

void MessageTransfer::recordBond(uint16_t conn, const uint8_t addr[6], uint8_t addrType)
{
    for (auto& bnd : bonded_) {
        if (bnd.used && bnd.conn == conn) {
            std::memcpy(bnd.addr, addr, 6);
            bnd.addrType = addrType;
            return;
        }
    }
    for (auto& bnd : bonded_) {
        if (!bnd.used) {
            bnd.used = true;
            bnd.conn = conn;
            std::memcpy(bnd.addr, addr, 6);
            bnd.addrType = addrType;
            return;
        }
    }
    // bonded_ is sized to the controller's MAX_CONNECTIONS; a full table would
    // mean a bond goes un-forgotten (security-relevant). Surface it.
    LOG_W(TAG, "bond table full; ephemeral bond may persist");
}

void MessageTransfer::forgetBondForConn(uint16_t conn)
{
    for (auto& bnd : bonded_) {
        if (bnd.used && bnd.conn == conn) {
            // Keep the LTK for a session-persistent peer so the next reconnect
            // re-encrypts silently; otherwise the pairing is ephemeral.
            if (!isSessionPeer(bnd.addr, bnd.addrType) && ble()) {
                ble()->forgetBond(bnd.addr, bnd.addrType);
            }
            bnd.used = false;
            return;
        }
    }
}

// =========================================================================
// Session-persistent pairings (runtime-only trust; NVS ledger is cleanup-only)
// =========================================================================

bool MessageTransfer::isSessionPeer(const uint8_t addr[6], uint8_t addrType) const
{
    for (const auto& p : sessionPeers_) {
        if (p.used && p.addrType == addrType && std::memcmp(p.addr, addr, 6) == 0) return true;
    }
    return false;
}

void MessageTransfer::rememberSessionPeer(const uint8_t addr[6], uint8_t addrType)
{
    if (isSessionPeer(addr, addrType)) return;
    for (auto& p : sessionPeers_) {
        if (!p.used) {
            p.used = true;
            std::memcpy(p.addr, addr, 6);
            p.addrType = addrType;
            ledgerDirty_ = true;  // flushed to NVS by tick() (host-task safe)
            LOG_I(TAG, "Session pairing remembered");
            return;
        }
    }
    LOG_W(TAG, "session-peer table full; pairing not remembered");
}

void MessageTransfer::forgetAllSessionPeers()
{
    auto* b = ble();
    bool any = false;
    for (auto& p : sessionPeers_) {
        if (p.used) {
            if (b) b->forgetBond(p.addr, p.addrType);
            p.used = false;
            any = true;
        }
    }
    if (any) {
        persistSessionLedger();  // writes empty -> erases the ledger key
        ledgerDirty_ = false;
    }
}

void MessageTransfer::persistSessionLedger()
{
    cdc::core::NvsScope nvs(kNsMsg, NVS_READWRITE);
    if (!nvs) return;
    uint8_t blob[1 + cdc::hal::IBluetoothController::MAX_BONDED_DEVICES * kLedgerEntryLen];
    uint8_t count = 0;
    for (const auto& p : sessionPeers_) {
        if (!p.used) continue;
        std::memcpy(&blob[1 + count * kLedgerEntryLen], p.addr, 6);
        blob[1 + count * kLedgerEntryLen + 6] = p.addrType;
        ++count;
    }
    if (count == 0) {
        nvs_erase_key(nvs, kKeyPeers);
    } else {
        blob[0] = count;
        nvs_set_blob(nvs, kKeyPeers, blob, 1 + count * kLedgerEntryLen);
    }
    nvs.commit();
}

void MessageTransfer::cleanupStaleSessionBonds()
{
    cdc::core::NvsScope nvs(kNsMsg, NVS_READWRITE);
    if (!nvs) return;
    uint8_t blob[1 + cdc::hal::IBluetoothController::MAX_BONDED_DEVICES * kLedgerEntryLen] = {};
    size_t len = sizeof(blob);
    if (nvs_get_blob(nvs, kKeyPeers, blob, &len) == ESP_OK && len >= 1) {
        const uint8_t count = blob[0];
        auto* b = ble();
        uint8_t cleared = 0;
        for (uint8_t i = 0; i < count && (1u + static_cast<size_t>(i + 1) * kLedgerEntryLen) <= len;
             ++i) {
            const uint8_t* a = &blob[1 + i * kLedgerEntryLen];
            const uint8_t t = blob[1 + i * kLedgerEntryLen + 6];
            if (b) b->forgetBond(a, t);
            ++cleared;
        }
        if (cleared > 0) LOG_I(TAG, "Cleared %u stale session pairing(s)", cleared);
    }
    nvs_erase_key(nvs, kKeyPeers);
    nvs.commit();
}

void MessageTransfer::storeResult(bool ok, bool wasSend, Reason reason, const char* mime,
                                  const char* peerName)
{
    lastResult_.ok = ok;
    lastResult_.wasSend = wasSend;
    lastResult_.reason = reason;
    std::strncpy(lastResult_.mime, mime ? mime : "", sizeof(lastResult_.mime) - 1);
    lastResult_.mime[sizeof(lastResult_.mime) - 1] = '\0';
    std::strncpy(lastResult_.peerName, peerName ? peerName : "", sizeof(lastResult_.peerName) - 1);
    lastResult_.peerName[sizeof(lastResult_.peerName) - 1] = '\0';
    lastResultValid_ = true;
}

// =========================================================================
// Main-loop tick
// =========================================================================

void MessageTransfer::tick(uint32_t /*nowMsArg*/)
{
    MutexGuard g(mutex_);

    // State start times are stamped with nowMs() from the UI/plugin call context,
    // which runs after the caller samples its loop timestamp. Comparing timeouts
    // against that caller timestamp can yield stateStartMs > now and underflow
    // now - stateStartMs, so sample the same monotonic clock here.
    const uint32_t now = nowMs();

    if (!bootApplied_) {
        bootApplied_ = true;
        auto* b = ble();
        if (beacon_.isEnabled() && b && !b->isEnabled()) {
            b->enable();
        }
    }

    // Boot janitor: once BLE is up, drop any session bonds left in the shared
    // NVS store by a previous run so runtime trust never survives a reboot.
    if (!sessionBondsCleaned_) {
        auto* b = ble();
        if (b && b->isEnabled()) {
            cleanupStaleSessionBonds();
            sessionBondsCleaned_ = true;
        }
    }

    // Flush a remembered pairing to the cleanup ledger off the host task.
    if (ledgerDirty_) {
        ledgerDirty_ = false;
        persistSessionLedger();
    }

    beacon_.reconcile();

    tickRecv(now);
    tickSend(now);
}

}  // namespace cdc::msg
