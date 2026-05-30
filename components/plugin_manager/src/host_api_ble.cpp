/**
 * \file host_api_ble.cpp
 * \brief BLE host API for plugins: read-only state, a single reserved GATT
 *        server service (peripheral), and the central (GATT client) role.
 *
 * Threading: the NimBLE stack invokes the GATT write / central callbacks on
 * its own task. Those callbacks never call into WASM; they copy the payload
 * into a mutex-protected ring and set a pending flag. plg_ble_pump() runs on
 * the plugin tick task (under the plugin call mutex) and fires the plugin's
 * action, which then pulls the payload with a host_ble_consume_* call - the
 * same deferred pattern the command channel uses.
 *
 * Only one plugin GATT service slot exists (the reserved BluetoothController
 * slot) and only one BLE connection exists at a time, so peripheral and
 * central state is single-instance rather than a per-plugin table.
 */

#include "cdc_hal/IBluetoothController.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>

using cdc::hal::IBluetoothController;
using cdc::hal::getBluetoothControllerInstance;
using cdc::hal::BleUuid;
using cdc::hal::GattServiceDef;
using cdc::hal::GattCharacteristic;
using cdc::hal::BleScanResult;
namespace pm = cdc::plugin_manager;

extern "C" void* plg_get_active_plugin(void);

namespace {

constexpr uint16_t BLE_MAX_PAYLOAD = 244;   // default-MTU payload budget
constexpr uint8_t  MAX_PLUGIN_CHARS = cdc::hal::IBluetoothController::MAX_CHARS_PER_SERVICE;
constexpr uint8_t  WRITE_RING = 4;
constexpr uint8_t  NOTIFY_RING = 4;
constexpr uint8_t  MAX_DISC_CHARS = 8;

// --- synchronisation between the NimBLE task and the plugin tick task ---
SemaphoreHandle_t s_lock = nullptr;
void lock_init() { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }
struct Guard {
    bool held = false;
    Guard()  { if (s_lock) held = (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE); }
    ~Guard() { if (held)  xSemaphoreGive(s_lock); }
};

// --- peripheral (GATT server) state ---
// NimBLE writes the assigned attribute handle here; it stays valid across a
// deferred (BLE-disabled) registration that is only committed at enable().
uint16_t s_value_handles[MAX_PLUGIN_CHARS];
struct PeriphChar {
    uint32_t write_action_id = 0;
};
struct {
    void*       plugin   = nullptr;
    bool        active   = false;
    uint8_t     uuid[16] = {};
    uint8_t     num_chars = 0;
    PeriphChar  chars[MAX_PLUGIN_CHARS];
} s_periph;

struct WriteEvt {
    uint32_t char_handle;
    uint16_t conn;
    uint16_t len;
    uint8_t  data[BLE_MAX_PAYLOAD];
};
WriteEvt s_wring[WRITE_RING];
volatile uint8_t s_wr_head = 0, s_wr_tail = 0;   // head=produced, tail=consumed

// Payload currently being delivered to a write action (tick-task only).
uint32_t s_cur_write_char = 0;
uint16_t s_cur_write_len  = 0;
uint8_t  s_cur_write_buf[BLE_MAX_PAYLOAD];

// --- central (GATT client) state ---
struct NotifyEvt { uint16_t value_handle; uint16_t len; uint8_t data[BLE_MAX_PAYLOAD]; };
struct {
    void*    plugin = nullptr;
    bool     listeners_registered = false;
    // Connection handle of the plugin's own central op; shared GATT-client
    // callbacks ignore events from any other connection (e.g. the system BLE
    // name resolver) so they don't clobber this state.
    uint16_t conn = 0xFFFF;
    uint32_t discover_action_id = 0;
    uint32_t read_action_id     = 0;
    uint32_t notify_action_id   = 0;

    // discovery result stash
    ble_remote_char_t disc[MAX_DISC_CHARS];
    uint8_t  disc_count = 0;
    bool     fire_discovery = false;

    // read result stash
    uint8_t  read_buf[BLE_MAX_PAYLOAD];
    uint16_t read_len = 0;
    bool     fire_read = false;

    // inbound notification ring
    NotifyEvt nring[NOTIFY_RING];
    uint8_t   n_head = 0, n_tail = 0;
} s_cen;

// --- helpers ---

IBluetoothController* ble() { return getBluetoothControllerInstance(); }

bool ble_allowed() {
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    return p && p->manifest().capabilities.ble;
}

// Build a 16-byte little-endian UUID array from a stack-independent BleUuid.
void uuid_to_bytes(const BleUuid& u, uint8_t out[16]) {
    if (u.type == BleUuid::UUID_128) {
        std::memcpy(out, u.u128, 16);
    } else {
        static const uint8_t base[16] = {
            0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        std::memcpy(out, base, 16);
        out[12] = static_cast<uint8_t>(u.u16 & 0xFF);
        out[13] = static_cast<uint8_t>(u.u16 >> 8);
    }
}

// Reserved system service UUIDs (little-endian, BleUuid::from128 order).
const uint8_t NUS_SVC[16]   = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                                0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e };
const uint8_t VCARD_SVC[16] = { 0x01,0x1A,0x8B,0x6A,0x9D,0x4C,0x6E,0x9A,
                                0x7A,0x4D,0x5D,0x8B,0x20,0x1F,0x2F,0x8E };
const uint8_t GPG_SVC[16]   = { 0x01,0x1A,0x8B,0x6A,0x9D,0x4C,0x6E,0x9A,
                                0x7A,0x4D,0x5D,0x8B,0x30,0x1F,0x2F,0x8E };

// Block the Bluetooth SIG 16-bit range (so a plugin can never shadow HID,
// Device Info, Battery, GAP, GATT, ...) and the system's 128-bit services.
bool uuid_is_reserved(const uint8_t uuid[16]) {
    static const uint8_t sig_base[12] = {
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00
    };
    if (std::memcmp(uuid, sig_base, 12) == 0 && uuid[14] == 0 && uuid[15] == 0) {
        return true;
    }
    return std::memcmp(uuid, NUS_SVC, 16) == 0
        || std::memcmp(uuid, VCARD_SVC, 16) == 0
        || std::memcmp(uuid, GPG_SVC, 16) == 0;
}

// --- central callback registration (host-side, fires on NimBLE/EventBus task) ---
void on_discovery(uint16_t connHandle, const IBluetoothController::DiscoveredService* svc, bool complete) {
    Guard g;
    if (connHandle != s_cen.conn) return;  // not our central op
    if (svc) {
        uint8_t n = svc->numCharacteristics;
        if (n > MAX_DISC_CHARS) n = MAX_DISC_CHARS;
        for (uint8_t i = 0; i < n; i++) {
            uuid_to_bytes(svc->characteristics[i].uuid, s_cen.disc[i].uuid);
            s_cen.disc[i].value_handle = svc->characteristics[i].valueHandle;
            s_cen.disc[i].properties   = svc->characteristics[i].properties;
            s_cen.disc[i].reserved     = 0;
        }
        s_cen.disc_count = n;
    }
    if (complete) s_cen.fire_discovery = true;
}

void on_char_read(uint16_t connHandle, uint16_t, const uint8_t* data, uint16_t len) {
    Guard g;
    if (connHandle != s_cen.conn) return;  // not our central op
    if (len > BLE_MAX_PAYLOAD) len = BLE_MAX_PAYLOAD;
    std::memcpy(s_cen.read_buf, data, len);
    s_cen.read_len  = len;
    s_cen.fire_read = true;
}

void on_notification(uint16_t, uint16_t attr, const uint8_t* data, uint16_t len) {
    Guard g;
    uint8_t next = static_cast<uint8_t>((s_cen.n_head + 1) % NOTIFY_RING);
    if (next == s_cen.n_tail) return;   // ring full: drop
    NotifyEvt& e = s_cen.nring[s_cen.n_head];
    if (len > BLE_MAX_PAYLOAD) len = BLE_MAX_PAYLOAD;
    e.value_handle = attr;
    e.len = len;
    std::memcpy(e.data, data, len);
    s_cen.n_head = next;
}

void ensure_central_listeners() {
    if (s_cen.listeners_registered) return;
    auto* b = ble();
    if (!b) return;
    b->addServiceDiscoveryCallback(on_discovery);
    b->addCharacteristicReadCallback(on_char_read);
    b->addNotificationCallback(on_notification);
    s_cen.listeners_registered = true;
}

}  // namespace

extern "C" {

/* ---------------------------------------------------------------- state -- */

bool host_ble_is_enabled(void) { auto* b = ble(); return b ? b->isEnabled() : false; }

int host_ble_mac(uint8_t out[6]) {
    if (!out) return HOST_ERR_INVALID_ARG;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    return b->getMacAddress(out) ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_device_name(char* out, size_t out_size) {
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    const char* name = b->getDeviceName();
    std::strncpy(out, name ? name : "", out_size - 1);
    out[out_size - 1] = '\0';
    return HOST_OK;
}

int8_t host_ble_rssi(void) { auto* b = ble(); return b ? b->getRssi() : 0; }

/* ----------------------------------------------------- peripheral (GATT) -- */

int host_ble_register_service(ble_service_def_t* def, ble_char_def_t* chars, uint32_t num_chars) {
    if (!def || !chars || num_chars == 0) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    if (num_chars > MAX_PLUGIN_CHARS) return HOST_ERR_INVALID_ARG;
    if (uuid_is_reserved(def->uuid)) return HOST_ERR_NO_CAPABILITY;

    void* plugin = plg_get_active_plugin();
    if (s_periph.active && s_periph.plugin != plugin) return HOST_ERR_BUSY;
    if (s_periph.active) {
        b->unregisterGattService(BleUuid::from128(s_periph.uuid));
        s_periph = {};
    }
    lock_init();

    GattCharacteristic gc[MAX_PLUGIN_CHARS] = {};
    for (uint32_t i = 0; i < num_chars; i++) {
        s_value_handles[i] = 0;
        gc[i].uuid        = BleUuid::from128(chars[i].uuid);
        gc[i].properties  = chars[i].properties;
        gc[i].permissions = 0;
        if (chars[i].properties & BLE_PROP_READ)  gc[i].permissions |= cdc::hal::GattPerm::READ;
        if (chars[i].properties & (BLE_PROP_WRITE | BLE_PROP_WRITE_NO_RSP))
            gc[i].permissions |= cdc::hal::GattPerm::WRITE;
        gc[i].valueHandle = &s_value_handles[i];
        const uint32_t char_handle = i + 1;
        if (chars[i].properties & (BLE_PROP_WRITE | BLE_PROP_WRITE_NO_RSP)) {
            gc[i].onWrite = [char_handle](uint16_t conn, uint16_t, const uint8_t* d, uint16_t l) -> int {
                Guard g;
                uint8_t next = static_cast<uint8_t>((s_wr_head + 1) % WRITE_RING);
                if (next == s_wr_tail) return 0;   // ring full: drop
                WriteEvt& e = s_wring[s_wr_head];
                if (l > BLE_MAX_PAYLOAD) l = BLE_MAX_PAYLOAD;
                e.char_handle = char_handle;
                e.conn = conn;
                e.len = l;
                std::memcpy(e.data, d, l);
                s_wr_head = next;
                return 0;
            };
        }
    }

    GattServiceDef svc{};
    svc.uuid = BleUuid::from128(def->uuid);
    svc.characteristics = gc;
    svc.numCharacteristics = static_cast<uint8_t>(num_chars);

    if (!b->registerGattService(svc, /*pluginReserved=*/true)) return HOST_ERR_BUSY;

    s_periph.plugin = plugin;
    s_periph.active = true;
    std::memcpy(s_periph.uuid, def->uuid, 16);
    s_periph.num_chars = static_cast<uint8_t>(num_chars);
    for (uint32_t i = 0; i < num_chars; i++) {
        s_periph.chars[i].write_action_id = chars[i].write_action_id;
        chars[i].char_handle = i + 1;
    }
    def->service_handle = 1;
    return HOST_OK;
}

int host_ble_unregister_service(uint32_t service_handle) {
    (void)service_handle;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!s_periph.active || s_periph.plugin != plg_get_active_plugin()) return HOST_ERR_NOT_FOUND;
    auto* b = ble();
    if (b) b->unregisterGattService(BleUuid::from128(s_periph.uuid));
    s_periph = {};
    return HOST_OK;
}

static int periph_send(uint32_t char_handle, const uint8_t* data, size_t len, bool indicate) {
    if (!data && len) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!s_periph.active || s_periph.plugin != plg_get_active_plugin()) return HOST_ERR_NOT_FOUND;
    if (char_handle < 1 || char_handle > s_periph.num_chars) return HOST_ERR_INVALID_ARG;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    uint16_t vh = s_value_handles[char_handle - 1];
    uint16_t conn = b->getConnectionHandle();
    if (conn == 0xFFFF) return HOST_ERR_NOT_FOUND;
    bool ok = indicate ? b->sendIndication(conn, vh, data, static_cast<uint16_t>(len))
                       : b->sendNotification(conn, vh, data, static_cast<uint16_t>(len));
    return ok ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_send_notification(uint32_t char_handle, const uint8_t* data, size_t len) {
    return periph_send(char_handle, data, len, false);
}

int host_ble_send_indication(uint32_t char_handle, const uint8_t* data, size_t len) {
    return periph_send(char_handle, data, len, true);
}

int host_ble_consume_write(uint32_t char_handle, uint8_t* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return HOST_ERR_INVALID_ARG;
    if (char_handle != s_cur_write_char) return 0;
    size_t n = s_cur_write_len;
    if (n > buf_size) n = buf_size;
    std::memcpy(buf, s_cur_write_buf, n);
    return static_cast<int>(n);
}

/* ---------------------------------------------------------- central role -- */

int host_ble_scan_start(uint32_t duration_ms) {
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    return b->startScan(duration_ms ? duration_ms : 5000) ? HOST_OK : HOST_ERR_GENERIC;
}

bool host_ble_scan_done(void) { auto* b = ble(); return b ? b->isScanComplete() : true; }

int host_ble_scan_results(ble_scan_result_t* out, size_t* count) {
    if (!out || !count) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    uint8_t cap = static_cast<uint8_t>(*count > 32 ? 32 : *count);
    BleScanResult tmp[32];
    uint8_t n = b->getScanResults(tmp, cap);
    for (uint8_t i = 0; i < n; i++) {
        std::memcpy(out[i].addr, tmp[i].mac, 6);
        out[i].addr_type = tmp[i].addrType;
        out[i].rssi = tmp[i].rssi;
        std::strncpy(out[i].name, tmp[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }
    *count = n;
    return HOST_OK;
}

int host_ble_connect(const uint8_t addr[6], uint8_t addr_type) {
    if (!addr) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    s_cen.plugin = plg_get_active_plugin();
    ensure_central_listeners();
    return b->connect(addr, addr_type) ? HOST_OK : HOST_ERR_GENERIC;
}

uint32_t host_ble_conn_handle(void) {
    auto* b = ble();
    if (!b) return 0;
    uint16_t h = b->getConnectionHandle();
    return h == 0xFFFF ? 0u : static_cast<uint32_t>(h);
}

int host_ble_disconnect(uint32_t conn) {
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    b->disconnectHandle(static_cast<uint16_t>(conn));
    return HOST_OK;
}

int host_ble_discover(uint32_t conn, const uint8_t uuid[16], uint32_t action_id) {
    if (!uuid) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    s_cen.plugin = plg_get_active_plugin();
    s_cen.conn = static_cast<uint16_t>(conn);
    s_cen.discover_action_id = action_id;
    ensure_central_listeners();
    return b->discoverServiceByUuid(static_cast<uint16_t>(conn), BleUuid::from128(uuid))
        ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_consume_discovery(ble_remote_char_t* out, size_t* count) {
    if (!out || !count) return HOST_ERR_INVALID_ARG;
    Guard g;
    uint8_t n = s_cen.disc_count;
    if (n > *count) n = static_cast<uint8_t>(*count);
    std::memcpy(out, s_cen.disc, n * sizeof(ble_remote_char_t));
    *count = n;
    return HOST_OK;
}

int host_ble_read_char(uint32_t conn, uint16_t value_handle, uint32_t action_id) {
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    s_cen.plugin = plg_get_active_plugin();
    s_cen.conn = static_cast<uint16_t>(conn);
    s_cen.read_action_id = action_id;
    ensure_central_listeners();
    return b->readCharacteristic(static_cast<uint16_t>(conn), value_handle)
        ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_consume_read(uint8_t* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return HOST_ERR_INVALID_ARG;
    Guard g;
    size_t n = s_cen.read_len;
    if (n > buf_size) n = buf_size;
    std::memcpy(buf, s_cen.read_buf, n);
    s_cen.read_len = 0;
    return static_cast<int>(n);
}

int host_ble_write_char(uint32_t conn, uint16_t value_handle,
                        const uint8_t* data, size_t len, uint8_t with_response) {
    if (!data && len) return HOST_ERR_INVALID_ARG;
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    return b->writeCharacteristic(static_cast<uint16_t>(conn), value_handle,
                                  data, static_cast<uint16_t>(len), with_response != 0)
        ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_subscribe(uint32_t conn, uint16_t cccd_handle, uint32_t action_id) {
    if (!ble_allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* b = ble();
    if (!b) return HOST_ERR_NOT_FOUND;
    s_cen.plugin = plg_get_active_plugin();
    s_cen.notify_action_id = action_id;
    ensure_central_listeners();
    return b->enableNotifications(static_cast<uint16_t>(conn), cccd_handle)
        ? HOST_OK : HOST_ERR_GENERIC;
}

int host_ble_consume_notification(uint16_t* value_handle_out, uint8_t* buf, size_t buf_size) {
    if (!value_handle_out || !buf || buf_size == 0) return HOST_ERR_INVALID_ARG;
    Guard g;
    if (s_cen.n_tail == s_cen.n_head) return HOST_ERR_NOT_FOUND;
    NotifyEvt& e = s_cen.nring[s_cen.n_tail];
    *value_handle_out = e.value_handle;
    size_t n = e.len;
    if (n > buf_size) n = buf_size;
    std::memcpy(buf, e.data, n);
    s_cen.n_tail = static_cast<uint8_t>((s_cen.n_tail + 1) % NOTIFY_RING);
    return static_cast<int>(n);
}

/* ------------------------------------------------ tick pump + unload hook -- */

// Drains queued BLE events on the plugin tick task and fires plugin actions.
void plg_ble_pump(void) {
    // Peripheral writes.
    for (;;) {
        uint32_t ch = 0, aid = 0;
        uint16_t conn = 0;
        {
            Guard g;
            if (s_wr_tail == s_wr_head) break;
            WriteEvt& e = s_wring[s_wr_tail];
            ch = e.char_handle;
            conn = e.conn;
            s_cur_write_len = e.len;
            std::memcpy(s_cur_write_buf, e.data, e.len);
            s_wr_tail = static_cast<uint8_t>((s_wr_tail + 1) % WRITE_RING);
        }
        if (s_periph.active && s_periph.plugin && ch >= 1 && ch <= s_periph.num_chars) {
            aid = s_periph.chars[ch - 1].write_action_id;
            s_cur_write_char = ch;
            if (aid) {
                pm::PluginManager::instance().dispatchActionTo(
                    static_cast<pm::Plugin*>(s_periph.plugin), aid, ch, conn);
            }
            s_cur_write_char = 0;
            s_cur_write_len = 0;
        }
    }

    // Central completions.
    if (!s_cen.plugin) return;
    bool fd, fr, have_notif;
    {
        Guard g;
        fd = s_cen.fire_discovery; s_cen.fire_discovery = false;
        fr = s_cen.fire_read;      s_cen.fire_read = false;
        have_notif = (s_cen.n_tail != s_cen.n_head);
    }
    auto* pl = static_cast<pm::Plugin*>(s_cen.plugin);
    auto& mgr = pm::PluginManager::instance();
    if (fd && s_cen.discover_action_id) mgr.dispatchActionTo(pl, s_cen.discover_action_id, 0, 0);
    if (fr && s_cen.read_action_id)     mgr.dispatchActionTo(pl, s_cen.read_action_id, 0, 0);
    if (have_notif && s_cen.notify_action_id) mgr.dispatchActionTo(pl, s_cen.notify_action_id, 0, 0);
}

// Drop any BLE resources owned by a plugin that is being unloaded.
void plg_ble_on_unload(void* plugin) {
    if (s_periph.active && s_periph.plugin == plugin) {
        auto* b = ble();
        if (b) b->unregisterGattService(BleUuid::from128(s_periph.uuid));
        s_periph = {};
    }
    if (s_cen.plugin == plugin) {
        s_cen.plugin = nullptr;
        s_cen.discover_action_id = s_cen.read_action_id = s_cen.notify_action_id = 0;
    }
}

}  // extern "C"
