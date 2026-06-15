/**
 * \file
 * \brief OS UI glue for the badge-to-badge message transfer framework:
 *        consent prompt, peer picker, progress view, beacon menu (toggle, name,
 *        scan). Observes cdc_msg via EventBus + polling.
 */

#include "AppUiInternal.h"
#include "cdc_msg/MessageTransfer.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_log.h"

#include <cstdio>
#include <cstring>

namespace cdc::ui {

namespace {

using cdc::msg::MessageTransfer;
using cdc::msg::PeerInfo;
using cdc::msg::SendState;

constexpr uint8_t  kMaxPeers = 12;
constexpr uint32_t kPeerRefreshMs = 1500;
constexpr uint32_t kPickerScanMs = 2500;

// ---- consent ----
InfoView* s_consentView = nullptr;
char      s_consentText[160];

// ---- peer picker / beacon scan ----
ListItem s_peerItems[kMaxPeers];
PeerInfo s_peers[kMaxPeers];
uint8_t  s_peerCount = 0;
bool     s_peerReadonly = false;
uint32_t s_peerLastRefreshMs = 0;
uint32_t s_peerFingerprint = 0;
char     s_peerTitle[32];

// ---- progress / picker request state ----
bool     s_progressIsSend = false;
bool     s_pickerPending = false;
uint32_t s_pickerStartMs = 0;

// Forward declarations for free functions used before their definitions.
void refreshPeers();
void openPeerView(bool readonly);
void pushProgressView(bool isSend);
void onPeerSelect(uint16_t index, void* userData);

// ===========================================================================
// Progress view
// ===========================================================================

class MsgProgressView : public ViewBase {
public:
    void setSend(bool isSend) { isSend_ = isSend; lastPct_ = 255; markDirty(); }

    void render(bool partial) override {
        (void)partial;
        hal::IDisplay* display = hal::getDisplayInstance();
        if (!display) return;
        auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
        if (!gfx) return;

        const uint16_t width = display->getWidth();
        const uint16_t height = display->getHeight();

        gfx->fillScreen(EPD_WHITE);
        gfx->setFont(nullptr);
        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);

        const char* title = isSend_ ? ui::tr("core.msg_sending") : ui::tr("core.msg_receiving");
        render::drawHeaderLeft(gfx, title, 6, 14, width);

        uint32_t total = 0;
        uint32_t done = isSend_ ? MessageTransfer::instance().sendProgress(&total)
                                : MessageTransfer::instance().recvProgress(&total);
        uint8_t pct = (total > 0) ? static_cast<uint8_t>((done * 100ULL) / total) : 0;

        const int barX = 10;
        const int barY = 55;
        const int barW = width - 20;
        const int barH = 18;
        render::drawDialogFrame(gfx, barX, barY, barW, barH);
        int fill = (barW - 4) * pct / 100;
        if (fill > 0) gfx->fillRect(barX + 2, barY + 2, fill, barH - 4, EPD_BLACK);

        char line[48];
        snprintf(line, sizeof(line), "%u%%  (%lu / %lu B)", pct,
                 static_cast<unsigned long>(done), static_cast<unsigned long>(total));
        gfx->setCursor(barX, barY + barH + 10);
        render::printText(gfx, line);

        render::drawFooterBar(gfx, width, height, nullptr, ui::tr("core.cancel"), true);
        lastPct_ = pct;
    }

    bool needsRender() const override { return dirty_; }

    void onTick(uint32_t /*nowMs*/) override {
        uint32_t total = 0;
        uint32_t done = isSend_ ? MessageTransfer::instance().sendProgress(&total)
                                : MessageTransfer::instance().recvProgress(&total);
        uint8_t pct = (total > 0) ? static_cast<uint8_t>((done * 100ULL) / total) : 0;
        if (pct != lastPct_) markDirty();
    }

    InputResult onKey(char key) override {
        if (key == 'N') {
            if (isSend_) MessageTransfer::instance().cancelSend();
            return InputResult::REQUEST_POP;
        }
        return InputResult::CONSUMED;
    }

    const char* getName() const override { return "MsgProgressView"; }

private:
    bool    isSend_ = false;
    uint8_t lastPct_ = 255;
};

MsgProgressView* s_progressView = nullptr;

// ===========================================================================
// Peer picker / beacon scan view
// ===========================================================================

/** \brief Renders one peer row: signal bars + name + RSSI. */
bool renderPeerRow(Gdey029T94* gfx, const ListItem& item, uint16_t index,
                   int x, int y, int w, int h, bool selected, void* userCtx) {
    (void)index; (void)h; (void)userCtx;
    if (!gfx || !item.userData) return false;
    const auto* p = reinterpret_cast<const PeerInfo*>(item.userData);
    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    int baseline = y + 6;
    drawSignalBars(gfx, x + 4, baseline - 6, p->rssi, selected);
    gfx->setCursor(x + 22, baseline);
    render::printText(gfx, p->name[0] ? p->name : "?");
    char rssiBuf[8];
    snprintf(rssiBuf, sizeof(rssiBuf), "%d", p->rssi);
    int16_t rx1, ry1; uint16_t rw, rh;
    gfx->getTextBounds(rssiBuf, 0, 0, &rx1, &ry1, &rw, &rh);
    gfx->setCursor(x + w - rw - 4, baseline);
    gfx->print(rssiBuf);
    return true;
}

class MsgPeerView : public ListView {
public:
    void onEnter(void* context) override {
        ListView::onEnter(context);
        startScanForMode();
        s_peerLastRefreshMs = 0;
    }
    void onResume() override {
        ListView::onResume();
        startScanForMode();
    }
    void onPause() override {
        // Covered by another view/modal: stop the continuous scan so it only runs
        // while the beacon-scan is actually visible. onResume() restarts it.
        if (s_peerReadonly) MessageTransfer::instance().stopDiscovery();
    }
    void onExit() override {
        if (s_peerReadonly) {
            MessageTransfer::instance().stopDiscovery();  // end the continuous scan
        } else if (MessageTransfer::instance().sendState() == SendState::PickingPeer) {
            // User backed out of an interactive send before picking; cancel it.
            MessageTransfer::instance().cancelSend();
        }
        ListView::onExit();
    }
    void onTick(uint32_t nowMs) override {
        if (nowMs - s_peerLastRefreshMs < kPeerRefreshMs) return;
        s_peerLastRefreshMs = nowMs;
        if (s_peerReadonly) {
            // Beacon scan: continuous multi-role scan. Poll results, and re-arm
            // the scan if it ever stopped (cancel race, controller hiccup).
            if (MessageTransfer::instance().discoveryDone()) {
                MessageTransfer::instance().startDiscovery(0, true);
            }
            refreshPeers();
        } else if (MessageTransfer::instance().discoveryDone()) {
            // Send picker: burst scan, restart each cycle.
            refreshPeers();
            MessageTransfer::instance().startDiscovery(kPeerRefreshMs * 4);
        }
    }
    const char* getName() const override { return "MsgPeerView"; }

private:
    // Beacon scan runs a continuous scan while the beacon keeps advertising
    // (so two scanning badges see each other); the send picker uses burst scans.
    static void startScanForMode() {
        if (s_peerReadonly) {
            MessageTransfer::instance().startDiscovery(0, true);
        } else {
            MessageTransfer::instance().startDiscovery(kPeerRefreshMs * 4);
        }
    }
};

MsgPeerView* s_peerView = nullptr;

/// Fingerprint of the current peer set (count + MACs). Excludes RSSI so the
/// e-paper is only redrawn when badges appear or disappear, not on every signal
/// fluctuation during the 6 s scan cycle.
uint32_t computePeerFingerprint() {
    uint32_t fp = s_peerCount;
    for (uint8_t i = 0; i < s_peerCount; ++i) {
        for (uint8_t b = 0; b < 6; ++b) fp = fp * 31u + s_peers[i].addr[b];
    }
    return fp;
}

void refreshPeers() {
    if (!s_peerView) return;
    s_peerCount = MessageTransfer::instance().getPeers(s_peers, kMaxPeers);
    uint32_t fp = computePeerFingerprint();
    if (fp == s_peerFingerprint) return;  // unchanged set, spare the e-paper
    s_peerFingerprint = fp;
    if (s_peerCount == 0) {
        s_peerItems[0] = {ui::tr("core.msg_searching"), 0, true, nullptr};
        s_peerView->init(s_peerTitle, s_peerItems, 1);
        return;
    }
    for (uint8_t i = 0; i < s_peerCount; ++i) {
        s_peerItems[i] = {s_peers[i].name[0] ? s_peers[i].name : "?", 0, false, &s_peers[i]};
    }
    s_peerView->init(s_peerTitle, s_peerItems, s_peerCount);
}

void onPeerSelect(uint16_t index, void* /*userData*/) {
    if (index >= s_peerCount) return;
    const PeerInfo& p = s_peers[index];
    if (s_peerReadonly) {
        char info[96];
        snprintf(info, sizeof(info), "%s\n\nRSSI: %d dBm", p.name[0] ? p.name : "?", p.rssi);
        showInfo(ui::tr("core.msg_beacon_scan"), info);
        return;
    }
    if (MessageTransfer::instance().confirmInteractiveTarget(p.addr, p.addrType)) {
        // Push (not replace) over the picker: ViewStack ticks only the top stack
        // view (+ top modal), so the picker beneath stops scanning until progress
        // pops. On success the completion handler pops the picker too.
        pushProgressView(true);
        ViewStack::instance().push(s_progressView);
    } else {
        showToastError(ui::tr("core.msg_transfer_fail"));
    }
}

void openPeerView(bool readonly) {
    s_peerReadonly = readonly;
    s_peerCount = 0;
    if (!s_peerView) {
        s_peerView = new MsgPeerView();
        s_peerView->setItemRenderer(renderPeerRow, nullptr);
        s_peerView->setOnSelect(onPeerSelect);
    }
    snprintf(s_peerTitle, sizeof(s_peerTitle), "%s",
             readonly ? ui::tr("core.msg_beacon_scan") : ui::tr("core.msg_pick_peer"));
    s_peerItems[0] = {ui::tr("core.msg_searching"), 0, true, nullptr};
    s_peerView->init(s_peerTitle, s_peerItems, 1);
    s_peerFingerprint = computePeerFingerprint();  // matches the empty placeholder
    ViewStack::instance().push(s_peerView);
}

void pushProgressView(bool isSend) {
    if (!s_progressView) s_progressView = new MsgProgressView();
    s_progressView->setSend(isSend);
    s_progressIsSend = isSend;
}

// ===========================================================================
// Consent prompt
// ===========================================================================

void onMsgConsentYes(void* /*ud*/) {
    ViewStack::instance().hideModal();
    MessageTransfer::instance().respondConsent(true);
    pushProgressView(false);
    ViewStack::instance().push(s_progressView);
}

void onMsgConsentNo(void* /*ud*/) {
    ViewStack::instance().hideModal();
    MessageTransfer::instance().respondConsent(false);
}

void onMsgConsentRequestEvent(const core::Event& /*evt*/) {
    // Receiving requires an ephemeral pairing, which the lock screen rejects
    // anyway; decline up front (mirrors the numeric-comparison pairing prompt)
    // so no consent modal or peer name surfaces over the lock screen.
    if (isBadgeLocked()) {
        MessageTransfer::instance().respondConsent(false);
        return;
    }
    char peerName[cdc::msg::kNameBufSize] = {};
    char mime[cdc::msg::kMimeBufSize] = {};
    const char* descKey = nullptr;
    uint32_t size = 0;
    if (!MessageTransfer::instance().getPendingConsent(peerName, sizeof(peerName), mime,
                                                       sizeof(mime), &descKey, &size)) {
        return;
    }
    const char* what = (descKey && descKey[0]) ? ui::tr(descKey) : mime;
    char fromLine[64];
    snprintf(fromLine, sizeof(fromLine), ui::tr("core.msg_offer_from"),
             peerName[0] ? peerName : "?");
    snprintf(s_consentText, sizeof(s_consentText), "%s\n\n%s\n%lu B",
             fromLine, what, static_cast<unsigned long>(size));

    if (!s_consentView) s_consentView = new InfoView();
    s_consentView->init(ui::tr("core.msg_offer_title"), s_consentText);
    s_consentView->setYesNoCallbacks(onMsgConsentYes, onMsgConsentNo, nullptr);
    ViewStack::instance().showModal(s_consentView);
}

void onMsgExchangeCompleteEvent(const core::Event& /*evt*/) {
    MessageTransfer::TransferResult res;
    bool have = MessageTransfer::instance().consumeResult(&res);

    if (s_progressView && ViewStack::instance().current() == s_progressView) {
        ViewStack::instance().pop();
        // For an interactive send, also drop the peer picker beneath so the user
        // returns to the module/plugin view instead of a re-scanning picker.
        if (have && res.wasSend && s_peerView &&
            ViewStack::instance().current() == static_cast<IView*>(s_peerView)) {
            ViewStack::instance().pop();
        }
    }
    if (have) {
        if (res.ok) {
            showToastSuccess(ui::tr("core.msg_transfer_ok"));
        } else if (res.reason == cdc::msg::Reason::UserDeclined) {
            showToastInfo(ui::tr("core.msg_declined"));
        } else if (res.reason == cdc::msg::Reason::NoHandler) {
            showToastError(ui::tr("core.msg_unsupported"));
        } else {
            showToastError(ui::tr("core.msg_transfer_fail"));
        }
    }
}

// ===========================================================================
// Beacon menu (Tools): on/off toggle, display name, scan
// ===========================================================================

ListView* s_beaconMenu = nullptr;
ListItem  s_beaconItems[3];

enum BeaconMenuIdx { BM_TOGGLE = 0, BM_NAME, BM_SCAN, BM_COUNT };

void rebuildBeaconMenu() {
    auto& m = MessageTransfer::instance();
    s_beaconItems[BM_TOGGLE] = {
        m.isBeaconEnabled() ? ui::tr("core.msg_beacon_on") : ui::tr("core.msg_beacon_off"),
        static_cast<uint8_t>(m.isBeaconActive() ? '*' : 0), false, nullptr};
    s_beaconItems[BM_NAME] = {ui::tr("core.msg_beacon_name"), 0, false, nullptr};
    s_beaconItems[BM_SCAN] = {ui::tr("core.msg_beacon_scan"), 0, false, nullptr};
    if (s_beaconMenu) s_beaconMenu->init(ui::tr("core.msg_beacon"), s_beaconItems, BM_COUNT);
}

void showBeaconNameSetup() {
    showT9Input(ui::tr("core.msg_beacon_name"),
                MessageTransfer::instance().getBeaconName(),
                [](const char* text) { MessageTransfer::instance().setBeaconName(text); },
                cdc::msg::kMaxNameLen);
}

void onBeaconMenuSelect(uint16_t index, void* /*userData*/) {
    switch (index) {
        case BM_TOGGLE: {
            auto& m = MessageTransfer::instance();
            m.setBeaconEnabled(!m.isBeaconEnabled());
            rebuildBeaconMenu();
            return;
        }
        case BM_NAME:
            showBeaconNameSetup();
            return;
        case BM_SCAN:
            showMsgBeaconScan();
            return;
    }
}

}  // namespace

// ===========================================================================
// Public entry points (declared in AppUiInternal.h)
// ===========================================================================

void showBeaconMenu() {
    if (!s_beaconMenu) {
        s_beaconMenu = new ListView();
        s_beaconMenu->setOnSelect(onBeaconMenuSelect);
    }
    rebuildBeaconMenu();
    ViewStack::instance().push(s_beaconMenu);
}

void msgTransferUiInit() {
    core::EventBus::instance().subscribe(
        onMsgConsentRequestEvent,
        core::EventBus::eventMask(core::EventType::BLE_CONSENT_REQUEST));
    core::EventBus::instance().subscribe(
        onMsgExchangeCompleteEvent,
        core::EventBus::eventMask(core::EventType::BLE_EXCHANGE_COMPLETE));
}

void msgTransferUiProcess(uint32_t nowMs) {
    if (MessageTransfer::instance().takeInteractiveRequest()) {
        s_pickerPending = true;
        s_pickerStartMs = nowMs;
        showToastInfo(ui::tr("core.ble_scanning"), 0);
    }
    if (s_pickerPending && (nowMs - s_pickerStartMs >= kPickerScanMs ||
                            MessageTransfer::instance().discoveryDone())) {
        s_pickerPending = false;
        ViewStack::instance().hideModal();  // dismiss the scanning toast
        openPeerView(false);
    }
}

void showMsgBeaconScan() {
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled()) {
        showToastError(ui::tr("core.hw_not_available"));
        return;
    }
    openPeerView(true);
}

}  // namespace cdc::ui
