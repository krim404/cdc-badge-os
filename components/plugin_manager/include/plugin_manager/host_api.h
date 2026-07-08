/**
 * \file host_api.h
 * \brief CDC Badge OS plugin host API - canonical C ABI contract.
 *
 * Mirrored from cdc-badge-os/components/plugin_manager/include/plugin_manager/host_api.h.
 * A CI job in both repositories detects drift between the two copies.
 *
 * All host functions are imported by the WASM plugin from the `cdc` module.
 * Plugin lifecycle exports (plugin_init, plugin_on_enter, ...) are declared
 * in plugin_lifecycle.h.
 */

#ifndef CDC_BADGE_HOST_API_H
#define CDC_BADGE_HOST_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* API Level                                                                 */
/* ------------------------------------------------------------------------- */

#define HOST_API_LEVEL_MAJOR  0
#define HOST_API_LEVEL_MINOR  8
#define HOST_API_LEVEL_STR    "0.8"
#define HOST_API_LEVEL_PACKED (((uint32_t)HOST_API_LEVEL_MAJOR << 16) | HOST_API_LEVEL_MINOR)

/* ------------------------------------------------------------------------- */
/* Return Codes                                                              */
/* ------------------------------------------------------------------------- */

#define HOST_OK                  0
#define HOST_ERR_GENERIC        -1
#define HOST_ERR_INVALID_ARG    -2
#define HOST_ERR_NO_CAPABILITY  -3
#define HOST_ERR_NOT_FOUND      -4
#define HOST_ERR_TIMEOUT        -5
#define HOST_ERR_NO_MEMORY      -6
#define HOST_ERR_BUSY           -7
#define HOST_ERR_NOT_SUPPORTED  -8
#define HOST_ERR_RMEM_FULL      -9

/**
 * \defgroup logging Logging
 * \brief Write structured log lines to the badge serial console.
 *
 * Output is routed through the firmware's cdc_log facility so it shows up
 * on the USB-CDC serial monitor. Use these instead of any platform-native
 * logging primitive.
 * \{
 */

#define LOG_LEVEL_ERROR   0
#define LOG_LEVEL_WARN    1
#define LOG_LEVEL_INFO    2
#define LOG_LEVEL_DEBUG   3
#define LOG_LEVEL_VERBOSE 4

/// \brief Write a single log line at the given level.
void host_log    (uint8_t level, const char* tag, const char* msg);

/// \brief Write a labelled hex dump of a binary buffer at debug level.
void host_log_hex(const char* tag, const char* label, const uint8_t* data, size_t len);

/** \} */

/**
 * \defgroup time Time / RTC
 * \brief Access monotonic uptime and the badge real-time clock.
 *
 * Use \ref host_uptime_ms for elapsed-time measurements; use the wall-clock
 * helpers (\ref host_unix_time, \ref host_local_time) only after checking
 * \ref host_is_time_set.
 * \{
 */

struct host_tm {
    uint16_t year;        /* 1900-3000 */
    uint8_t  month;       /* 1-12 */
    uint8_t  day;         /* 1-31 */
    uint8_t  hour;        /* 0-23 */
    uint8_t  minute;      /* 0-59 */
    uint8_t  second;      /* 0-59 */
    uint8_t  weekday;     /* 0=Sunday */
};

/// \brief Monotonic milliseconds since boot.
uint64_t host_uptime_ms        (void);

/// \brief Current Unix timestamp in seconds, or 0 if RTC not set.
int64_t  host_unix_time        (void);

/// \brief Fill `out` with the current local time broken into fields.
int      host_local_time       (struct host_tm* out);

/// \brief Configured timezone offset from UTC in seconds.
int32_t  host_timezone_offset  (void);

/// \brief True when the RTC has been synchronised at least once.
bool     host_is_time_set      (void);

/** \} */

/**
 * \defgroup power Power
 * \brief Battery state, USB connection and charger status.
 * \{
 */

#define POWER_SRC_UNKNOWN  0
#define POWER_SRC_BATTERY  1
#define POWER_SRC_USB      2

#define CHARGE_NOT_CHARGING 0
#define CHARGE_PRE_CHARGE   1
#define CHARGE_FAST         2
#define CHARGE_DONE         3
#define CHARGE_FAULT        4

/// \brief Battery voltage in millivolts.
uint16_t host_battery_mv         (void);

/// \brief Battery state of charge as 0..100 percent.
uint8_t  host_battery_pct        (void);

/// \brief True when USB VBUS is detected.
bool     host_is_usb_connected   (void);

/// \brief Active power source - one of POWER_SRC_*.
uint8_t  host_power_source       (void);

/// \brief Charger state machine value - one of CHARGE_*.
uint8_t  host_charge_status      (void);

/// \brief True when battery has crossed the low-warning threshold.
bool     host_is_battery_low     (void);

/// \brief True when battery has crossed the critical-shutdown threshold.
bool     host_is_battery_critical(void);

/// \brief Hold or release a light-sleep inhibitor for the calling plugin.
///        While any inhibitor is held the badge does not enter light sleep,
///        so a background plugin keeps ticking. Keyed by the plugin id and
///        released automatically when the plugin is unloaded.
/// \param on Non-zero to hold the inhibitor, zero to release it.
void     host_set_sleep_inhibit  (uint32_t on);

/** \} */

/**
 * \defgroup crypto Crypto
 * \brief Software crypto primitives and binary-to-text codecs.
 *
 * Hashing, AEAD and RNG. Asymmetric key operations live in the
 * \ref secure_element group instead (they live in TROPIC01).
 * \{
 */

/// \brief Fill `buf` with hardware-RNG bytes; may fall back to PRNG.
int host_random        (uint8_t* buf, size_t len);

/// \brief Fill `buf` with hardware-RNG bytes only; fails without TRNG.
int host_random_strict (uint8_t* buf, size_t len);

/// \brief SHA-256 hash of `data` into the 32-byte `out`.
int host_sha256        (const uint8_t* data, size_t len, uint8_t out[32]);

/// \brief HMAC-SHA-256 of `data` under `key` into the 32-byte `out`.
int host_hmac_sha256   (const uint8_t* key, size_t klen,
                        const uint8_t* data, size_t dlen, uint8_t out[32]);

/**
 * \brief AES-256-GCM encrypt.
 * \param key 32-byte key.
 * \param iv 12-byte nonce.
 * \param aad Additional authenticated data (may be NULL when aad_len == 0).
 * \param pt Plaintext input of pt_len bytes.
 * \param ct Ciphertext output buffer of at least pt_len bytes.
 * \param tag 16-byte authentication tag output.
 */
int host_aes_gcm_encrypt(const uint8_t* key, const uint8_t* iv,
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* pt, size_t pt_len,
                         uint8_t* ct, uint8_t tag[16]);

/**
 * \brief AES-256-GCM decrypt and verify.
 * \param key 32-byte key.
 * \param iv 12-byte nonce.
 * \param aad Additional authenticated data (may be NULL when aad_len == 0).
 * \param ct Ciphertext input of ct_len bytes.
 * \param tag 16-byte tag to verify.
 * \param pt Plaintext output buffer of at least ct_len bytes.
 */
int host_aes_gcm_decrypt(const uint8_t* key, const uint8_t* iv,
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* ct, size_t ct_len,
                         const uint8_t tag[16], uint8_t* pt);

/// \brief Base32-encode `in` into NUL-terminated `out`.
int host_base32_encode(const uint8_t* in, size_t in_len, char* out, size_t out_size);

/// \brief Base32-decode `in` into raw bytes in `out`.
int host_base32_decode(const char* in, size_t in_len, uint8_t* out, size_t out_size);

/// \brief Base64-encode `in` into NUL-terminated `out`.
int host_base64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_size);

/// \brief Base64-decode `in` into raw bytes in `out`.
int host_base64_decode(const char* in, size_t in_len, uint8_t* out, size_t out_size);

/// \brief Lowercase-hex-encode `in` into NUL-terminated `out`.
int host_hex_encode   (const uint8_t* in, size_t in_len, char* out, size_t out_size);

/// \brief Hex-decode `in` (case-insensitive) into raw bytes in `out`.
int host_hex_decode   (const char* in, size_t in_len, uint8_t* out, size_t out_size);

/** \} */

/**
 * \defgroup secure_element SecureElement / TROPIC01
 * \brief ECC key slots and retained-memory storage on the TROPIC01.
 *
 * ECC slots hold private keys that never leave the chip. Retained-memory
 * (rmem) slots offer persistent named storage shared across reboots and
 * plugin reinstalls; declare names in the plugin manifest under
 * `capabilities.rmem`.
 * \{
 */

#define ECC_CURVE_P256    0
#define ECC_CURVE_ED25519 1

/*
 * Retained memory (rmem) is allocated by name from a shared plugin pool.
 * Slots persist across reboot, plugin uninstall, and plugin reinstall as
 * long as the name remains declared in some installed plugin's
 * `capabilities.rmem`. Plugins declaring the same name share the same
 * physical slot (intentional, common scope).
 *
 * Name length: max 15 bytes excluding the trailing NUL.
 */

#define HOST_RMEM_NAME_MAX 15

/**
 * \brief Read a named retained-memory slot.
 * \param name NUL-terminated name, max HOST_RMEM_NAME_MAX bytes.
 * \param buf Output buffer.
 * \param len In: capacity of buf; out: bytes actually read.
 */
int      host_rmem_read_named  (const char* name, uint8_t* buf, size_t* len);

/// \brief Write up to host_rmem_slot_size() bytes into a named rmem slot.
int      host_rmem_write_named (const char* name, const uint8_t* buf, size_t len);

/// \brief Erase the contents of a named rmem slot.
int      host_rmem_erase_named (const char* name);

/// \brief True if the named rmem slot currently holds data.
bool     host_rmem_name_used   (const char* name);

/// \brief Maximum payload bytes per rmem slot.
uint16_t host_rmem_slot_size   (void);

/*
 * ECC keys are addressed by name, not by physical slot. The host maps each
 * declared name (manifest `capabilities.ecc`) to a slot in a reserved plugin
 * ECC pool and persists the mapping in NVS, so a key keeps its slot across
 * reboot and reinstall. The pool is small (ECC slots are scarce and reserved
 * for firmware features such as attestation and WebAuthn); growing it is a
 * firmware change.
 *
 * Name length: max HOST_ECC_NAME_MAX bytes excluding the trailing NUL.
 */
#define HOST_ECC_NAME_MAX 15

/// \brief Generate a fresh ECC key for the named slot.
int      host_ecc_generate (const char* name, uint8_t curve);

/// \brief Import an externally-generated private key for the named slot.
int      host_ecc_import   (const char* name, const uint8_t* priv, uint8_t curve);

/// \brief Export the public key for the named slot.
int      host_ecc_pubkey   (const char* name, uint8_t* pub, uint8_t curve);

/// \brief Erase the named ECC key and free its pool slot.
int      host_ecc_delete   (const char* name);

/// \brief True when the named ECC key currently holds a key.
bool     host_ecc_exists   (const char* name);

/// \brief ECDSA-sign `msg` with the P-256 named key; writes 64-byte raw sig.
int      host_ecdsa_sign   (const char* name, const uint8_t* msg, size_t len, uint8_t sig[64]);

/// \brief Ed25519-sign `msg` with the named key; writes 64-byte signature.
int      host_eddsa_sign   (const char* name, const uint8_t* msg, size_t len, uint8_t sig[64]);

/**
 * \brief Read the TROPIC01 chip serial / identity blob.
 * \param serial Output buffer.
 * \param len In: capacity; out: bytes written.
 */
int      host_se_chip_id   (uint8_t* serial, size_t* len);

/// \brief Read TROPIC01 firmware versions for the RISC-V CPU and SPECT core.
int      host_se_fw_version(uint8_t* riscv, uint8_t* spect);

/** \} */

/**
 * \defgroup http HTTP (streamed)
 * \brief Make outbound HTTP/HTTPS requests with streaming response reads.
 *
 * Opens a handle, lets you stage headers/body, performs the request, then
 * streams the response in chunks. Always close the handle when done, even
 * after errors. Requires manifest capability "http".
 * \{
 */

#define HTTP_GET    0
#define HTTP_POST   1
#define HTTP_PUT    2
#define HTTP_DELETE 3

/**
 * \brief Open an HTTP request.
 * \param method One of HTTP_GET/POST/PUT/DELETE.
 * \param url Absolute URL including scheme.
 * \param timeout_ms Connect + response timeout.
 * \return Handle > 0 on success, negative HostError on failure.
 */
int    host_http_open          (uint8_t method, const char* url, uint32_t timeout_ms);

/// \brief Add a request header before perform().
int    host_http_set_header    (int handle, const char* key, const char* value);

/// \brief Stage a request body before perform().
int    host_http_set_body      (int handle, const uint8_t* body, size_t len);

/// \brief Send the request and read response headers.
int    host_http_perform       (int handle);

/// \brief HTTP response status code, or negative on error.
int    host_http_status        (int handle);

/**
 * \brief Stream one response chunk into `buf`.
 * \param out_len Bytes actually read; 0 indicates end of response.
 */
int    host_http_read_chunk    (int handle, uint8_t* buf, size_t buf_size, size_t* out_len);

/// \brief Response Content-Length, or 0 when unknown / chunked.
size_t host_http_content_length(int handle);

/// \brief Release a request handle.
int    host_http_close         (int handle);

/** \} */

/**
 * \defgroup socket Socket (TCP / UDP client)
 * \brief Generic outbound byte-stream / datagram transport for plugin protocols.
 *
 * Opens a connection to a single remote endpoint, reads/writes bytes, and
 * closes the handle. Both protocols are connected: UDP fixes the peer at open
 * time, so write/read behave like the TCP path (no per-call address). Requires
 * manifest capability "socket" and an active network connection. The host owns
 * DNS resolution, timeouts, socket limits, and cleanup of leaked handles when a
 * plugin unloads.
 * \{
 */

/** \brief Protocol selector for \ref host_socket_open. */
#define HOST_SOCK_TCP  0
#define HOST_SOCK_UDP  1

/**
 * \brief Open an outbound connection to a single remote endpoint.
 * \param proto HOST_SOCK_TCP or HOST_SOCK_UDP.
 * \param host Hostname or numeric IP address.
 * \param port Remote port.
 * \param timeout_ms Connect timeout (TCP handshake; ignored for UDP).
 * \return Handle > 0 on success, negative HostError on failure.
 */
int host_socket_open(uint8_t proto, const char* host, uint16_t port, uint32_t timeout_ms);

/**
 * \brief Write bytes to the stream / send a datagram to the connected peer.
 * \return Number of bytes written, or negative HostError on failure.
 */
int host_socket_write(int handle, const uint8_t* data, size_t len, uint32_t timeout_ms);

/**
 * \brief Read bytes from the stream / receive a datagram from the connected peer.
 * \return Number of bytes read, 0 on EOF (TCP), or negative HostError on failure.
 */
int host_socket_read(int handle, uint8_t* out, size_t cap, uint32_t timeout_ms);

/// \brief Close a socket handle.
int host_socket_close(int handle);

/** \} */

/**
 * \defgroup net Inbound TCP listener (server)
 * \brief Accept inbound TCP connections on a plugin-chosen port.
 *
 * The client socket API is outbound-only; this adds the server side. The plugin
 * picks the port; the firmware binds/listens/accepts on a dedicated task and
 * fires `action_id` while connections wait. The handler calls
 * \ref host_net_accept to take the next connection as a socket handle, then
 * drives it with the normal \ref host_socket_read / \ref host_socket_write /
 * \ref host_socket_close (no extra `socket` capability needed for an accepted
 * connection). Requires manifest capability "net_listen". WiFi must be up
 * (host_wifi_request) for clients to reach the badge; the listener itself binds
 * regardless and simply waits. One listener per plugin. The listen socket is
 * transparently re-opened after a network drop across light sleep.
 * \{
 */

/// \brief Start a TCP listener on `port`; fires `action_id` while clients wait.
/// \return HOST_OK, HOST_ERR_NO_CAPABILITY, HOST_ERR_INVALID_ARG, HOST_ERR_BUSY,
///         HOST_ERR_NO_MEMORY.
int host_net_listen(uint16_t port, uint32_t action_id);

/// \brief Take the next accepted connection as a socket handle for
///        host_socket_read/write/close.
/// \return Socket handle (>= 1), or HOST_ERR_NOT_FOUND when none is pending.
int host_net_accept(void);

/// \brief Stop the listener (pass 0 or the listening port). Closes pending
///        un-accepted connections.
/// \return HOST_OK or HOST_ERR_NOT_FOUND.
int host_net_close(uint16_t port);

/** \} */

/**
 * \defgroup lifecycle Plugin lifecycle
 * \brief Opt-in background residency.
 *
 * The `background` and `autoload` capabilities only grant permission to stay
 * resident; they do not by themselves keep the plugin loaded. A plugin must
 * call host_set_resident(true) to actually remain in the background (typically
 * in plugin_init for an autoload service, or while running for a background
 * one). Without it, the plugin is torn down when the user leaves it, and an
 * autoload plugin is unloaded right after boot init. Pass false to opt back out.
 * \{
 */

/// \brief Request (true) or drop (false) background residency. Needs the
///        `background` or `autoload` capability to have any effect.
/// \return HOST_OK, or HOST_ERR_GENERIC if called with no active plugin.
int host_set_resident(bool resident);

/** \} */

/**
 * \defgroup wifi WiFi
 * \brief Request WiFi STA mode and inspect connection state.
 *
 * The WiFi radio is shared - acquire it with \ref host_wifi_request before
 * use and release it as early as possible. Requires manifest capability
 * "wifi".
 * \{
 */

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;
    uint8_t channel;
    uint8_t auth_mode;
} wifi_scan_result_t;

/// \brief Request the shared WiFi radio and wait up to `timeout_ms` for join.
int     host_wifi_request    (uint32_t timeout_ms);

/// \brief Release the WiFi radio held by this plugin.
int     host_wifi_release    (void);

/// \brief True when WiFi STA is associated and has an IP.
bool    host_wifi_is_connected(void);

/// \brief Copy the currently joined SSID into `out`.
int     host_wifi_ssid       (char* out, size_t out_size);

/// \brief Copy the current IPv4 address as dotted decimal into `out`.
int     host_wifi_ip         (char* out, size_t out_size);

/// \brief Current AP signal strength in dBm.
int8_t  host_wifi_rssi       (void);

/// \brief Read the station MAC address.
int     host_wifi_mac        (uint8_t out[6]);

/// \brief Start an asynchronous WiFi scan.
int     host_wifi_start_scan (void);

/// \brief True when the scan started by host_wifi_start_scan has finished.
bool    host_wifi_scan_done  (void);

/**
 * \brief Read results from the last completed scan.
 * \param count In: capacity of `out`; out: number of entries written.
 */
int     host_wifi_scan_results(wifi_scan_result_t* out, size_t* count);

/** \} */

/**
 * \defgroup ble BLE
 * \brief Bluetooth Low Energy peripheral and central operations.
 *
 * Plugins can publish GATT services as a peripheral and, with the
 * appropriate capability, scan and talk to remote devices as a central.
 * \{
 */

/* GATT characteristic property flags (BLE Core Spec Vol 3, Part G, 3.3.1.1). */
#define BLE_PROP_READ          0x02
#define BLE_PROP_WRITE_NO_RSP  0x04
#define BLE_PROP_WRITE         0x08
#define BLE_PROP_NOTIFY        0x10
#define BLE_PROP_INDICATE      0x20

/** \brief One characteristic of a plugin GATT service (peripheral role). */
typedef struct {
    uint8_t  uuid[16];        /**< 128-bit characteristic UUID. */
    uint8_t  properties;      /**< BLE_PROP_* bitmask. */
    uint8_t  reserved[3];
    uint32_t write_action_id; /**< Action fired on each inbound write (0 = none). */
    uint32_t char_handle;     /**< OUT: handle for notify / indicate / consume_write. */
} ble_char_def_t;

/** \brief A plugin GATT service definition (peripheral role). Always primary. */
typedef struct {
    uint8_t  uuid[16];        /**< 128-bit primary service UUID. */
    uint8_t  num_chars;       /**< Number of entries in the chars array (1..6). */
    uint8_t  reserved[3];
    uint32_t service_handle;  /**< OUT: handle for unregister. */
} ble_service_def_t;

/** \brief One device from a central scan. */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;        /**< 0 = public, 1 = random. */
    int8_t  rssi;
    char    name[32];
} ble_scan_result_t;

/** \brief One characteristic discovered on a connected peer (central role). */
typedef struct {
    uint8_t  uuid[16];
    uint16_t value_handle;
    uint8_t  properties;      /**< BLE_PROP_* bitmask. */
    uint8_t  reserved;
} ble_remote_char_t;

/* --- State (read-only) --- */

/// \brief True when the BLE stack is initialised and advertising or connectable.
bool    host_ble_is_enabled       (void);

/// \brief Read the local BLE MAC address.
int     host_ble_mac              (uint8_t out[6]);

/// \brief Copy the local BLE device name into `out`.
int     host_ble_device_name      (char* out, size_t out_size);

/// \brief Signal strength of the active BLE link in dBm, or 0 when idle.
int8_t  host_ble_rssi             (void);

/* --- Peripheral (GATT server) --- */

/**
 * \brief Register the plugin's GATT service and its characteristics.
 *
 * Fills `def->service_handle` and each `chars[i].char_handle`. The service UUID
 * must not be a reserved system UUID and a plugin service slot must be free.
 * \param def       Service definition; `service_handle` is written back.
 * \param chars     Characteristic array; `char_handle` is written back per entry.
 * \param num_chars Number of characteristics (1..6).
 */
int     host_ble_register_service (ble_service_def_t* def,
                                   ble_char_def_t* chars, uint32_t num_chars);

/// \brief Tear down the plugin's registered GATT service.
int     host_ble_unregister_service(uint32_t service_handle);

/// \brief Notify subscribers of a value on one of the plugin's characteristics.
int     host_ble_send_notification(uint32_t char_handle, const uint8_t* data, size_t len);

/// \brief Indicate (acknowledged notify) a value on a plugin characteristic.
int     host_ble_send_indication  (uint32_t char_handle, const uint8_t* data, size_t len);

/**
 * \brief Pull the next queued inbound write for `char_handle`.
 *
 * Call from the characteristic's `write_action_id` handler; the action fires
 * with `idx` set to the characteristic handle and `user_data` to the
 * connection handle.
 * \return Bytes copied (>= 0), or a negative HOST_ERR_* code.
 */
int     host_ble_consume_write    (uint32_t char_handle, uint8_t* buf, size_t buf_size);

/* --- Central (GATT client) --- */

/// \brief Start a central scan for `duration_ms` milliseconds.
int     host_ble_scan_start       (uint32_t duration_ms);

/// \brief True when the scan started by host_ble_scan_start() has finished.
bool    host_ble_scan_done        (void);

/**
 * \brief Read results from the last central scan.
 * \param count In: capacity of `out`; out: entries written.
 */
int     host_ble_scan_results     (ble_scan_result_t* out, size_t* count);

/**
 * \brief Connect to a peer. Completion arrives as a BLE_CONNECTED event; read
 *        the resulting handle with host_ble_conn_handle().
 */
int     host_ble_connect          (const uint8_t addr[6], uint8_t addr_type);

/// \brief Current connection handle (central or peripheral), or 0 when idle.
uint32_t host_ble_conn_handle     (void);

/// \brief Disconnect a connection.
int     host_ble_disconnect       (uint32_t conn);

/**
 * \brief Discover the characteristics of one service on a connected peer.
 *        Completion fires `action_id`; read entries with host_ble_consume_discovery().
 */
int     host_ble_discover         (uint32_t conn, const uint8_t uuid[16], uint32_t action_id);

/**
 * \brief Pull discovered characteristics after a discovery action fires.
 * \param count In: capacity of `out`; out: entries written.
 */
int     host_ble_consume_discovery(ble_remote_char_t* out, size_t* count);

/**
 * \brief Start reading a peer characteristic by value handle. Completion fires
 *        `action_id`; read the value with host_ble_consume_read().
 */
int     host_ble_read_char        (uint32_t conn, uint16_t value_handle, uint32_t action_id);

/**
 * \brief Pull the value delivered by the last read action.
 * \return Bytes copied (>= 0), or a negative HOST_ERR_* code.
 */
int     host_ble_consume_read     (uint8_t* buf, size_t buf_size);

/// \brief Write a value to a peer characteristic by value handle.
int     host_ble_write_char       (uint32_t conn, uint16_t value_handle,
                                   const uint8_t* data, size_t len, uint8_t with_response);

/**
 * \brief Subscribe to notifications on a peer characteristic (by CCCD handle).
 *        Each notification fires `action_id`; read it with
 *        host_ble_consume_notification(). Low-level variant: the caller must
 *        already know the CCCD handle. Prefer \ref host_ble_subscribe_char,
 *        which resolves the CCCD via descriptor discovery.
 */
int     host_ble_subscribe        (uint32_t conn, uint16_t cccd_handle, uint32_t action_id);

/**
 * \brief Subscribe to notifications on a peer characteristic by VALUE handle.
 *
 * Discovers the characteristic's CCCD descriptor host-side and writes it
 * (falling back to value_handle + 1 when the peer exposes no 0x2902).
 * Call after host_ble_discover() on the same connection - the last discovered
 * service bounds the descriptor search. Each notification fires `action_id`;
 * read it with host_ble_consume_notification().
 */
int     host_ble_subscribe_char   (uint32_t conn, uint16_t value_handle, uint32_t action_id);

/**
 * \brief Pull the next queued inbound notification.
 * \param value_handle_out Receives the source characteristic value handle.
 * \return Bytes copied (>= 0), or a negative HOST_ERR_* code.
 */
int     host_ble_consume_notification(uint16_t* value_handle_out, uint8_t* buf, size_t buf_size);

/**
 * \brief Usable ATT payload of the current connection (negotiated MTU - 3).
 * \param conn Connection handle (currently ignored - single connection).
 * \return Payload bytes per write/notification, or 0 when not connected /
 *         no capability.
 */
uint16_t host_ble_get_mtu         (uint32_t conn);

/**
 * \brief Register an action fired on every completed central write.
 *
 * Fires `plugin_on_action(action_id, attr_handle, status)` per completed
 * write (both with-response completions and CCCD writes; status 0 = success).
 * Use it to pace chunked transfers instead of writing blind. Pass 0 to
 * unregister.
 */
int     host_ble_on_write_complete(uint32_t action_id);

/** \} */

/**
 * \defgroup nvs NVS (plugin-namespaced)
 * \brief Persistent key/value storage scoped to the calling plugin.
 *
 * Keys live in an NVS namespace derived from the plugin id, so different
 * plugins cannot collide. Erasing the plugin wipes its namespace.
 * \{
 */

/**
 * \brief Read a binary blob from NVS.
 * \param len In: capacity of `buf`; out: bytes actually read.
 */
int host_nvs_get_blob (const char* key, uint8_t* buf, size_t* len);

/// \brief Write a binary blob to NVS.
int host_nvs_set_blob (const char* key, const uint8_t* buf, size_t len);

/// \brief Read a uint32 value.
int host_nvs_get_u32  (const char* key, uint32_t* out);

/// \brief Write a uint32 value.
int host_nvs_set_u32  (const char* key, uint32_t value);

/// \brief Read a NUL-terminated string.
int host_nvs_get_str  (const char* key, char* buf, size_t buf_size);

/// \brief Write a NUL-terminated string.
int host_nvs_set_str  (const char* key, const char* value);

/// \brief Delete a single key.
int host_nvs_erase    (const char* key);

/// \brief Erase every key in the plugin's namespace.
int host_nvs_erase_all(void);

/**
 * \brief Enumerate the keys in the plugin's namespace.
 * \param out_len In: capacity of `out`; out: bytes written (NUL-separated list).
 */
int host_nvs_list_keys(char* out, size_t* out_len);

/** \} */

/**
 * \defgroup vfat vFAT file storage (plugin-sandboxed)
 * \brief Sandboxed file access on the plugins FAT partition.
 *
 * Each plugin can only touch files in its own private folder; the host builds
 * and confines every path. Requires the `vfat` capability. `name` is a bare
 * filename: characters [A-Za-z0-9._-], no path separators, no leading dot,
 * at most 64 bytes.
 * \{
 */

/// \brief Create or overwrite `name` with `len` bytes. \return HOST_OK or HOST_ERR_*.
int host_fs_write (const char* name, const uint8_t* data, size_t len);

/**
 * \brief Read `name` into `buf`.
 * \param len In: capacity of `buf`; out: bytes actually read.
 * \return HOST_OK, HOST_ERR_NOT_FOUND, or another HOST_ERR_*.
 */
int host_fs_read  (const char* name, uint8_t* buf, size_t* len);

/// \brief Delete `name`. \return HOST_OK or HOST_ERR_NOT_FOUND.
int host_fs_remove(const char* name);

/// \brief Write the byte size of `name` to `*out`. \return HOST_OK or HOST_ERR_NOT_FOUND.
int host_fs_size  (const char* name, size_t* out);

/**
 * \brief Enumerate the plugin's own files.
 * \param out_len In: capacity of `out`; out: bytes written ('\n'-separated list).
 */
int host_fs_list  (char* out, size_t* out_len);

/**
 * \brief Open one of the plugin's own files in a scrollable on-screen text
 *        viewer (same as opening the file in the vFAT explorer). Useful for a
 *        bundled readme / help page. \return HOST_OK or a HOST_ERR_* code.
 */
int host_fs_view  (const char* name);

/**
 * \brief Decode and show one of the plugin's own image files (PNG/JPEG) on the
 *        e-paper, dithered and scaled to fit. \return HOST_OK or a HOST_ERR_* code.
 */
int host_fs_view_image  (const char* name);

/**
 * \brief Render and show one of the plugin's own Markdown files in the
 *        scrollable text viewer. \return HOST_OK or a HOST_ERR_* code.
 */
int host_fs_view_markdown  (const char* name);

/** \} */

/**
 * \defgroup ui_views UI - Views
 * \brief Push prebuilt UI views onto the system view stack.
 *
 * Plugins drive the UI by pushing high-level views (toasts, lists, T9
 * inputs, sliders, ...) and reacting to action callbacks. Input results
 * are read back with \ref host_ui_consume_input_text or
 * \ref host_ui_consume_input_int.
 * \{
 */

typedef struct {
    const char* label;
    uint8_t     icon;
    bool        icon_disabled;
    uint32_t    item_id;
} ui_item_t;

/* CP437 glyph codes routed straight to Adafruit-GFX. cdc_log + plugin SDKs
 * mirror these. UI_ICON_NONE renders a default bullet point. */
#define UI_ICON_NONE            0
#define UI_ICON_SUCCESS         2
#define UI_ICON_ERROR           1
#define UI_ICON_HEART           3    /* favorite                           */
#define UI_ICON_DIAMOND         4
#define UI_ICON_CLUB            5
#define UI_ICON_SPADE           6
#define UI_ICON_BULLET          7
#define UI_ICON_INVERSE_BULLET  8    /* remove / delete                    */
#define UI_ICON_CIRCLE          9    /* info / hollow circle               */
#define UI_ICON_INVERSE_CIRCLE  0x0A
#define UI_ICON_MALE            0x0B
#define UI_ICON_FEMALE          0x0C
#define UI_ICON_MUSIC           0x0D
#define UI_ICON_NOTES           0x0E /* scene / playlist                   */
#define UI_ICON_SUN             0x0F /* brightness / light                 */
#define UI_ICON_PLAY            0x10 /* action / switch / submenu          */
#define UI_ICON_REVERSE_PLAY    0x11 /* back                               */
#define UI_ICON_UPDOWN          0x12 /* cover / vertical adjust            */
#define UI_ICON_ALERT           0x13 /* double exclamation                 */
#define UI_ICON_PARAGRAPH       0x14
#define UI_ICON_SECTION         0x15
#define UI_ICON_BAR             0x16 /* sensor / list / count              */
#define UI_ICON_UPDOWN_BAR      0x17
#define UI_ICON_ARROW_UP        0x18
#define UI_ICON_ARROW_DOWN      0x19
#define UI_ICON_ARROW_RIGHT     0x1A
#define UI_ICON_ARROW_LEFT      0x1B
#define UI_ICON_ANGLE           0x1C
#define UI_ICON_LEFTRIGHT       0x1D
#define UI_ICON_TRIANGLE_UP     0x1E
#define UI_ICON_TRIANGLE_DOWN   0x1F

/* Semantic aliases. Plugins are free to use either the CP437 name or
 * a semantic one - they map to the same byte. */
#define UI_ICON_INFO    UI_ICON_CIRCLE
#define UI_ICON_TASK    UI_ICON_PLAY
#define UI_ICON_REMOVE  UI_ICON_INVERSE_BULLET
#define UI_ICON_LIGHT   UI_ICON_SUN
#define UI_ICON_COVER   UI_ICON_UPDOWN
#define UI_ICON_SENSOR  UI_ICON_BAR
#define UI_ICON_SWITCH  UI_ICON_PLAY
#define UI_ICON_SCENE   UI_ICON_NOTES
#define UI_ICON_BACK    UI_ICON_REVERSE_PLAY

/// \brief Show a transient toast overlay.
int host_ui_push_toast      (const char* text, uint8_t icon, uint16_t duration_ms);

/// \brief Show a blocking message view that auto-dismisses after `duration_ms`.
int host_ui_push_message    (const char* text, uint8_t icon, uint32_t duration_ms);

/**
 * \brief Show a Y/N confirmation.
 * \param action_id Fired with user_data = 1 on Y, user_data = 0 on N (idx unused).
 */
int host_ui_push_confirm    (const char* text, uint8_t icon, uint32_t action_id);

/// \brief Show a scrollable info screen with title and body.
int host_ui_push_info       (const char* title, const char* body);

/**
 * \brief Decode and show an image (PNG/JPEG) from an in-memory buffer, dithered.
 *        No capability required; the buffer is bounds-checked.
 * \param data Encoded image bytes.
 * \param len Byte length (rejected above 512 KB).
 */
int host_ui_view_image      (const uint8_t* data, uint32_t len);

/**
 * \brief Render and show Markdown from an in-memory (UTF-8) buffer.
 *        No capability required; the buffer is bounds-checked.
 * \param data Markdown bytes.
 * \param len Byte length (truncated at ~64 KB).
 */
int host_ui_view_markdown   (const uint8_t* data, uint32_t len);

/**
 * \brief Open a URL in the badge browser (enters the browser and loads it).
 *        No capability required. \return HOST_OK, HOST_ERR_INVALID_ARG, or
 *        HOST_ERR_NOT_SUPPORTED when the browser module is not present.
 * \param url Target URL (UTF-8, NUL-terminated).
 */
int host_browser_open       (const char* url);

/**
 * \brief Show a context menu. At most 8 items (ContextMenuView::MAX_ITEMS); the
 *        menu is scrollable and shows 4 at a time.
 * \param select_action_id Fired on selection with idx = selected item position
 *        (0-based) and user_data = items[i].item_id.
 * \return HOST_OK, or HOST_ERR_INVALID_ARG when count is 0 or greater than the
 *         8-item limit. Plugins must keep menus within that limit.
 */
int host_ui_push_context_menu(const char* title, const ui_item_t* items, uint16_t count,
                              uint32_t select_action_id);

/**
 * \brief Show a T9-style text entry.
 * \param initial Pre-filled text, or NULL/empty for blank.
 * \param action_id Fired on both outcomes; the view pops itself before it
 *        fires. On confirm: user_data = 1, idx = entered text length, read the
 *        text via host_ui_consume_input_text. On cancel: user_data = 0, idx = 0
 *        and no text is pending (host_ui_consume_input_text returns nothing).
 */
int host_ui_push_t9_input   (const char* title, const char* initial,
                             uint16_t max_len, uint32_t action_id);

/**
 * \brief Show a password entry (masked T9).
 * \param initial Pre-filled text, or NULL/empty for blank.
 * \param action_id Same contract as host_ui_push_t9_input: confirm fires with
 *        user_data = 1 and idx = text length; cancel fires with user_data = 0.
 */
int host_ui_push_password   (const char* title, const char* initial,
                             uint16_t max_len, uint32_t action_id);

/**
 * \brief Show a numeric PIN entry.
 * \param max_attempts 0 for unlimited.
 * \param action_id On confirm fires with user_data = 1 and idx = PIN length;
 *        on cancel fires with user_data = 0. The view pops itself first.
 */
int host_ui_push_pin_entry  (const char* title, uint8_t max_len, uint8_t max_attempts,
                             uint32_t action_id);

/* On confirm fires action_id with user_data = 1 (read the value via
 * host_ui_consume_input_int); on cancel fires with user_data = 0 and no value
 * pending. The view pops itself before the action fires. */
/// \brief Show an integer slider.
int host_ui_push_slider     (const char* title, int32_t min, int32_t max, int32_t init,
                             int32_t step, const char* unit, uint32_t action_id);

/* RGB color picker. On Y the host fires action_id with idx = packed RGB
 * (0xRRGGBB) and user_data = 1; on N it fires with user_data = 0 and no value
 * pending. The view pops itself first. Read the packed value via
 * host_ui_consume_input_int(). */
/// \brief Show an RGB color picker.
int host_ui_push_color_picker(uint8_t initial_r, uint8_t initial_g, uint8_t initial_b,
                              uint32_t action_id);

/* On confirm fires action_id with user_data = 1 (read via
 * host_ui_consume_input_int); on cancel fires with user_data = 0. Pops first. */
/// \brief Show a date picker.
int host_ui_push_date       (const char* title, uint8_t d, uint8_t m, uint16_t y,
                             uint32_t action_id);

/* On confirm fires action_id with user_data = 1 (read via
 * host_ui_consume_input_int); on cancel fires with user_data = 0. Pops first. */
/// \brief Show a time-of-day picker.
int host_ui_push_time       (const char* title, uint8_t h, uint8_t m, uint32_t action_id);

/**
 * \brief Show a list view.
 * \param select_action_id Fired on item select with idx = selected row index
 *        and user_data = items[i].item_id.
 * \param menu_action_id Fired when the user opens the per-item context menu
 *        (same idx/user_data as select); 0 to disable.
 */
int host_ui_push_list       (const char* title, const ui_item_t* items, uint16_t count,
                             uint32_t select_action_id, uint32_t menu_action_id);

/* Replace the plugin's currently-top list view with a fresh one. Falls back
 * to a plain push when the plugin has no list on top (e.g. on first call).
 * Use this for "refresh after toggle" patterns so the view stack does not
 * grow on every action. */
/// \brief Replace the plugin's top list view in place; falls back to push when none.
int host_ui_replace_list    (const char* title, const ui_item_t* items, uint16_t count,
                             uint32_t select_action_id, uint32_t menu_action_id);

/* Override the footer hint for the plugin's current top view (list, confirm,
 * T9, pin, slider, date or time). Pass NULL or an empty string to fall back
 * to the view's default hint. The text is copied internally so the caller
 * can free it after the call returns. Returns HOST_ERR_NOT_FOUND if the top
 * view is not owned by the plugin runtime. */
/// \brief Override the footer hint of the plugin's current top view.
int host_ui_set_view_footer (const char* hint);

/// \brief Override the empty-state text shown by an empty list view.
int host_ui_set_view_empty  (const char* text);

/* Wire hide/show callbacks on the plugin's current top view. `hide_action_id`
 * fires via plugin_on_action(hide_action_id, 0, 0) when the view is covered by
 * another view or modal; `show_action_id` fires when it becomes visible again.
 * Pass 0 for either id to leave that event unhooked. Returns HOST_ERR_NOT_FOUND
 * if the top view is not owned by the plugin runtime. */
/// \brief Register hide/show callbacks for the plugin's current top view.
int host_ui_set_view_lifecycle(uint32_t hide_action_id, uint32_t show_action_id);

/* Redraw a single row of the plugin's current top list view in place, without
 * re-pushing or fully re-rendering the list. The label, icon and item_id of
 * the given index are replaced from `item`; the new label is copied
 * internally. Only repaints when the plugin's list is the active top view and
 * the row is on screen (partial refresh). Returns HOST_ERR_NOT_FOUND if the
 * top view is not the plugin's list, HOST_ERR_INVALID_ARG on a bad index. */
/// \brief Update one list row in place (partial redraw).
int host_ui_update_list_item(uint16_t index, const ui_item_t* item);

/* Insert a new row into the plugin's current top list at `index` (later rows
 * shift down), then partial-repaint. The label is copied internally. `index`
 * is clamped to the current item count (append). Returns HOST_ERR_NOT_FOUND if
 * the top view is not the plugin's list. */
/// \brief Insert a list row at `index` (partial redraw).
int host_ui_insert_list_item(uint16_t index, const ui_item_t* item);

/* Remove the row at `index` from the plugin's current top list (later rows
 * shift up), then partial-repaint. Returns HOST_ERR_NOT_FOUND if the top view
 * is not the plugin's list, HOST_ERR_INVALID_ARG on a bad index. */
/// \brief Remove the list row at `index` (partial redraw).
int host_ui_remove_list_item(uint16_t index);

/// \brief Pop the topmost view.
int host_ui_pop                (void);

/**
 * \brief Pop back to the plugin's first view.
 *
 * Pops every view the plugin pushed during or after plugin_on_enter, back to
 * its first view (the stack depth recorded at entry). Views below the plugin
 * are untouched. No-op if the plugin pushed nothing.
 */
int host_ui_pop_to_plugin      (void);

/// \brief Force a repaint of the current view.
int host_ui_repaint            (void);

/// \brief Read text input committed by the most recent input view.
int host_ui_consume_input_text (char* out, size_t out_size);

/// \brief Read integer input committed by the most recent input view.
int host_ui_consume_input_int  (int32_t* out);

/// \brief Claim exclusive UI ownership (block other plugins from pushing views).
int host_ui_acquire_exclusive  (void);

/// \brief Release a previously acquired exclusive UI lock.
int host_ui_release_exclusive  (void);

/**
 * \brief Arm an inactivity timer for the plugin's current view.
 * \param action_id Fired when no input arrives within `timeout_ms`.
 */
int host_ui_set_inactivity     (uint32_t timeout_ms, uint32_t action_id);

/*
 * Blink the badge backlight as a visual "look at me" signal. Count is clamped
 * to 1..10, period_ms (each off- and on-phase) to 50..1000. Use 0 for either
 * argument to take the default (2 cycles, 150 ms). Blocks the calling task
 * for `2 * count * period_ms` milliseconds; the underlying LEDC PWM is
 * thread-safe so no framebuffer ordering is involved.
 */
/**
 * \brief Blink the backlight as a visual identification signal.
 * \param count Number of on/off cycles, clamped to 1..10 (0 = default 2).
 * \param period_ms Duration of each phase, clamped to 50..1000 (0 = default 150).
 */
int host_ui_wink               (uint8_t count, uint16_t period_ms);

/** \} */

/**
 * \defgroup ui_canvas UI - Canvas view
 * \brief Plugin-drawn custom views with inline interactive widgets.
 *
 * Push one canvas, then issue draw commands and add widgets that the host
 * routes key events to. Commit when ready to refresh the display.
 * \{
 */

/* Canvas widget event subtypes used as the user_data on the widget callback. */
#define CANVAS_WIDGET_CHANGED   1
#define CANVAS_WIDGET_COMMITTED 2
#define CANVAS_WIDGET_CANCELLED 3

/**
 * \brief Push a new canvas view.
 * \param key_action_id Fired on raw key events not consumed by a focused
 *        widget, with idx = focused widget id and user_data = the ASCII key code.
 * \param widget_action_id Fired for widget interaction events with idx = widget
 *        id and user_data = the event subtype (see CANVAS_WIDGET_*).
 */
int host_view_canvas_push          (const char* title, uint32_t key_action_id,
                                    uint32_t widget_action_id);

/// \brief Read the drawable body region (excluding header/footer).
int host_view_canvas_get_body_size (uint16_t* w, uint16_t* h);

/// \brief Override the footer hint of the canvas.
int host_view_canvas_set_footer    (const char* hint);

/// \brief Clear all draw state and widgets. Equals host_view_canvas_clear_ex(0).
int host_view_canvas_clear         (void);

/* Flags for host_view_canvas_clear_ex(). */
#define HOST_CANVAS_CLEAR_KEEP_SPRITES 0x01 /**< Sprite assets survive the clear. */

/**
 * \brief Clear with options: like \ref host_view_canvas_clear, but
 *        HOST_CANVAS_CLEAR_KEEP_SPRITES keeps the sprite store intact.
 *
 * Kept sprites retain frames, masks, flags, scale and the current frame
 * index, so a plugin can create its sheets once and rebuild screens around
 * them. Their PLAYBACK is stopped (an orphaned loop such as a marquee's
 * backing sprite would otherwise keep the animation clock and the panel
 * busy with nothing to show) - call \ref host_sprite_play again after
 * re-recording the new screen. Elements, tweens and widgets are dropped
 * either way.
 * \return HOST_OK, HOST_ERR_INVALID_ARG (unknown flag),
 *         HOST_ERR_NOT_FOUND (no canvas).
 */
int host_view_canvas_clear_ex      (uint32_t flags);

/// \brief Set text size multiplier (Adafruit-GFX semantics).
int host_view_canvas_set_text_size (uint8_t size);

#define HOST_FONT_BUILTIN   0  ///< Adafruit-GFX 6x8; CP437 codepoints for umlauts.
#define HOST_FONT_BOLD_9PT  1  ///< FreeMonoBold 9pt; Latin-1 indexed.
#define HOST_FONT_BOLD_12PT 2  ///< FreeMonoBold 12pt; Latin-1 indexed.
#define HOST_FONT_BOLD_18PT 3  ///< FreeMonoBold 18pt; ASCII only.
#define HOST_FONT_BOLD_24PT 4  ///< FreeMonoBold 24pt; ASCII only.
#define HOST_FONT_COUNT     5  ///< Number of defined font ids.

/**
 * \brief Switch the canvas font to one of the canonical HOST_FONT_* ids.
 *
 * Persists across draw calls until the next \ref host_view_canvas_clear or
 * a further set_font call. All text drawing functions take UTF-8; the host
 * renders umlauts correctly for whichever font is active, both the builtin
 * 6x8 font (HOST_FONT_BUILTIN) and the Latin-1-indexed FreeMonoBold fonts.
 *
 * \param font_id One of HOST_FONT_*.
 * \return HOST_OK on success, HOST_ERR_INVALID_ARG for out-of-range ids.
 */
int host_view_canvas_set_font      (uint8_t font_id);

/**
 * \brief Pick the largest HOST_FONT_* whose rendered \p text fits within
 *        \p max_width_px. Candidates are evaluated in array order; sort
 *        them from largest to smallest. Falls back to the last entry when
 *        nothing fits.
 *
 * Pure measurement; does not change canvas state. Pair with
 * \ref host_view_canvas_set_font to apply the picked font.
 *
 * \param text Null-terminated string to measure.
 * \param max_width_px Pixel budget.
 * \param candidates Array of HOST_FONT_* ids.
 * \param count Number of entries in \p candidates.
 * \param out_font_id Receives the chosen font id.
 * \return HOST_OK on success, HOST_ERR_INVALID_ARG for empty input.
 */
int host_text_pick_font_that_fits  (const char* text, int16_t max_width_px,
                                    const uint8_t* candidates, uint32_t count,
                                    uint8_t* out_font_id);

/// \brief Switch between normal and inverted (white on black) text.
int host_view_canvas_set_text_color(bool inverted);

/**
 * \brief Set the fill ink for subsequent filled shapes (rect, circle, triangle).
 *
 * 0 = none (nothing drawn), 255 = solid black (default). Values in between are
 * rendered as an ordered-dither grey approximation (8x8 Bayer, ~64 levels) so a
 * 1-bpp panel can fake greyscale fills. Outlines, lines, text and bitmaps are
 * unaffected and always solid.
 * \param shade Fill ink level, 0 (white) .. 255 (solid black).
 */
int host_view_canvas_set_shade     (uint8_t shade);

/// \brief Draw text at (x, y) using the current text size/colour.
int host_view_canvas_draw_text     (int16_t x, int16_t y, const char* text);

/**
 * \brief Draw text within a horizontal box.
 * \param align 0 = left, 1 = center, 2 = right.
 */
int host_view_canvas_draw_text_aligned(int16_t x, int16_t y, int16_t w,
                                       const char* text, uint8_t align);

/// \brief Draw a rectangle outline or filled rectangle.
int host_view_canvas_draw_rect     (int16_t x, int16_t y, int16_t w, int16_t h, bool filled);

/// \brief Draw a single pixel.
int host_view_canvas_draw_pixel    (int16_t x, int16_t y);

/// \brief Draw a line between two points.
int host_view_canvas_draw_line     (int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/// \brief Draw a circle outline or filled circle of radius r centred at (x, y).
int host_view_canvas_draw_circle   (int16_t x, int16_t y, int16_t r, bool filled);

/// \brief Draw a triangle outline or filled triangle through three points.
int host_view_canvas_draw_triangle (int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                    int16_t x2, int16_t y2, bool filled);

/// \brief Draw a rounded rectangle outline or filled, corner radius r.
int host_view_canvas_draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                    int16_t r, bool filled);

/**
 * \brief Draw a 1-bpp bitmap; set bits render black, unset bits are transparent.
 *
 * Rows are byte-padded (stride = (w + 7) / 8), MSB first per Adafruit-GFX
 * convention. The pixel data is copied into the canvas, so the buffer may be
 * reused after the call.
 * \param data Packed 1-bpp pixel data, length stride * h.
 * \param len Length of \p data in bytes.
 */
int host_view_canvas_draw_bitmap   (int16_t x, int16_t y, int16_t w, int16_t h,
                                    const uint8_t* data, uint32_t len);

/// \brief Draw a horizontal line.
int host_view_canvas_hline         (int16_t x, int16_t y, int16_t w);

/// \brief Draw a vertical line.
int host_view_canvas_vline         (int16_t x, int16_t y, int16_t h);

/**
 * \brief Flush draw state to the panel.
 * \param full_refresh true to force a full e-paper refresh, false for partial.
 */
int host_view_canvas_commit        (bool full_refresh);

/**
 * \brief Start recording draw calls under element `elem_id`.
 *
 * Elements group draw commands so they can later be moved, hidden or removed
 * without rebuilding the whole display list. They are the building block for
 * animations: record an element once, then per frame adjust its offset and
 * call \ref host_view_canvas_commit. The element is created on first use;
 * calling begin again with the same id appends further commands to it.
 * Element ids live in their own namespace (unrelated to widget ids); at most
 * 16 elements can exist at once. \ref host_view_canvas_clear drops all
 * elements together with the display list.
 *
 * \param elem_id Non-zero element id chosen by the plugin.
 * \return HOST_OK, HOST_ERR_INVALID_ARG (id 0), HOST_ERR_NO_MEMORY (element
 *         table full), HOST_ERR_NOT_FOUND (no canvas).
 */
int host_view_canvas_elem_begin    (uint32_t elem_id);

/**
 * \brief Stop recording draw calls into an element.
 *
 * Subsequent draw calls are untagged (static background). Ending is implicit
 * when \ref host_view_canvas_elem_begin switches to another element.
 */
int host_view_canvas_elem_end      (void);

/**
 * \brief Set an element's replay offset relative to its recorded coordinates.
 *
 * The offset is applied when the display list is replayed; call
 * \ref host_view_canvas_commit afterwards to show the change. (0, 0) restores
 * the recorded position.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_set_offset(uint32_t elem_id, int16_t ox, int16_t oy);

/**
 * \brief Shift an element's replay offset by a delta.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_move     (uint32_t elem_id, int16_t dx, int16_t dy);

/**
 * \brief Show or hide an element (hidden elements are skipped on replay).
 *
 * Hiding keeps the element's draw commands recorded, so it can be shown again
 * cheaply (blink patterns). Call \ref host_view_canvas_commit afterwards.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_show     (uint32_t elem_id, bool visible);

/**
 * \brief Remove an element and all draw commands recorded under it.
 *
 * Display-list slots and arena bytes are reclaimed, so elements can be
 * removed and re-recorded repeatedly (sprite whose content changes). Call
 * \ref host_view_canvas_commit afterwards.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_remove   (uint32_t elem_id);

/**
 * \brief Drop only an element's recorded draw commands, keeping the element.
 *
 * Offset, visibility, z layer and running tweens survive; follow with
 * \ref host_view_canvas_elem_begin plus draw calls to re-record the content
 * in place (live counters, changing labels). Arena bytes are reclaimed.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_clear    (uint32_t elem_id);

/**
 * \brief Set an element's replay layer (z-order).
 *
 * Layers draw in ascending order; untagged draw commands and untouched
 * elements live in layer 0. Ties keep recording order, so existing plugins
 * are unaffected until they opt in.
 * \param z Layer, -128..127 (default 0).
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown element or no canvas).
 */
int host_view_canvas_elem_set_z    (uint32_t elem_id, int8_t z);

/// \brief Read an element's current replay offset.
int host_view_canvas_elem_get_offset(uint32_t elem_id, int16_t* ox, int16_t* oy);

/**
 * \brief Bounding box of an element's recorded commands, offset applied.
 *
 * Body-local pixels; useful for edge/collision checks while animating.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown/empty element or no canvas).
 */
int host_view_canvas_elem_get_bounds(uint32_t elem_id, int16_t* x, int16_t* y,
                                     uint16_t* w, uint16_t* h);

/* Animation refresh policies for host_view_canvas_set_anim_policy(). */
#define HOST_CANVAS_ANIM_REFRESH_AUTO  0 /* flash-free while running, one FAST
                                            cleanup ~1 s after going idle    */
#define HOST_CANVAS_ANIM_REFRESH_LIGHT 1 /* never clean up automatically;
                                            plugin manages ghosting itself   */

/**
 * \brief Configure host-driven animation pacing for the current canvas.
 *
 * While tweens or sprite playback are active the host advances them, commits
 * the canvas automatically and keeps partial refreshes flash-free (no
 * escalation mid-motion). Under AUTO (default) the panel gets one FAST
 * cleanup refresh about a second after the last animation ends, and endless
 * animations get a rare hygiene FAST to bound ghosting.
 * \param refresh_policy One of HOST_CANVAS_ANIM_REFRESH_*.
 * \param max_fps Animation step-rate cap 1..5 (0 = default 4). The panel's
 *        partial waveform (~250 ms) makes ~5 the physical ceiling; steps are
 *        computed from elapsed time, so slow refreshes drop frames instead of
 *        slowing motion down. Think in tween durations of 500 ms and up.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_NOT_FOUND (no canvas).
 */
int host_view_canvas_set_anim_policy(uint8_t refresh_policy, uint8_t max_fps);

/**
 * \brief Record a draw of a sprite's current frame at (x, y).
 *
 * Draw-by-reference: one display-list slot, no pixel copy; frame changes from
 * \ref host_sprite_play or \ref host_sprite_set_frame replay automatically.
 * Record it inside an element to move, hide, layer or tween the sprite.
 * Destroying the sprite later leaves the command recorded but skipped.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown sprite or no canvas),
 *         HOST_ERR_NO_MEMORY (display list full).
 */
int host_view_canvas_draw_sprite   (int16_t x, int16_t y, uint32_t sprite);

/**
 * \brief Draw subsequent shapes in white instead of black.
 *
 * Applies to rects, circles, triangles, round-rects, lines and pixels
 * recorded afterwards - an eraser for wipe transitions and cut-outs over
 * previously drawn content (dither shades erase dithered). Text keeps its
 * own \ref host_view_canvas_set_text_color. Reset by
 * \ref host_view_canvas_clear.
 * \param white 1 = white ink, 0 = black (default).
 */
int host_view_canvas_set_ink       (bool white);

/**
 * \brief Record a host-driven text marquee (ticker).
 *
 * The text is rendered once with the current font/size into an internal
 * sprite; a `window_w`-wide window then scrolls through it seamlessly on the
 * host clock (content + a window-sized gap, wrap-around). No per-frame
 * plugin code and no display-list churn. Record inside an element to move,
 * hide or layer the ticker.
 * \param step_px Pixels advanced per animation beat (pair with the beat
 *        duration `frame_ms`, floored at 50).
 * \return Backing sprite handle >= 1 (pause via host_sprite_stop, remove via
 *         host_sprite_destroy), HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY,
 *         HOST_ERR_NOT_FOUND (no canvas).
 */
int host_view_canvas_marquee       (int16_t x, int16_t y, int16_t window_w,
                                    const char* text, uint16_t step_px,
                                    uint16_t frame_ms);

/// \brief Add an integer slider widget bound to `widget_id`.
int host_view_canvas_add_slider    (uint32_t widget_id, int32_t min, int32_t max,
                                    int32_t initial, int32_t step);

/// \brief Add a T9 text input widget bound to `widget_id`.
int host_view_canvas_add_text      (uint32_t widget_id, uint16_t max_len, const char* initial);

/// \brief Add a focusable button widget bound to `widget_id`.
int host_view_canvas_add_button    (uint32_t widget_id);

/// \brief Remove a widget previously added to the canvas.
int host_view_canvas_remove_widget (uint32_t widget_id);

/// \brief Set the integer value of a slider widget.
int host_view_canvas_set_value     (uint32_t widget_id, int32_t value);

/// \brief Read the integer value of a slider widget.
int host_view_canvas_get_value     (uint32_t widget_id, int32_t* out);

/// \brief Set the text of a text-input widget.
int host_view_canvas_set_text      (uint32_t widget_id, const char* text);

/// \brief Read the text of a text-input widget.
int host_view_canvas_get_text      (uint32_t widget_id, char* out, size_t cap);

/// \brief Move keyboard focus to the given widget.
int host_view_canvas_set_focus     (uint32_t widget_id);

/// \brief Read the currently focused widget id, 0 if none.
int host_view_canvas_get_focus     (uint32_t* out);

/**
 * \brief Configure key auto-repeat timing for the canvas.
 * \param initial_ms Delay before the first repeat.
 * \param repeat_ms Period between subsequent repeats.
 */
int host_view_canvas_set_key_repeat(uint16_t initial_ms, uint16_t repeat_ms);

/**
 * \brief Set the action id fired on a canvas long-press.
 *
 * Registering a non-zero action opts the canvas into deferred short-press
 * input: a tap fires the key callback on release while a hold (>= long-press
 * threshold) fires this action with idx = 0 and user_data = the ASCII key code,
 * and suppresses the short press. Pass 0 to disable.
 * \param action_id Action fired on long-press, or 0 to disable.
 */
int host_view_canvas_set_long_press_action(uint32_t action_id);

/** \} */

/**
 * \defgroup canvas_anim UI - Canvas animation
 * \brief Host-driven tweens on canvas elements.
 *
 * A tween animates an element's replay offset over time with an easing curve;
 * the host advances it on its own clock, commits the canvas and fires
 * `plugin_on_action(done_action_id, anim_handle, elem_id)` on completion - no
 * per-frame plugin code needed. Sequencing is expressed with `delay_ms`
 * (parallel staggering) and `start_after` (chains); repetition with `repeat`
 * and the YOYO flag. See \ref host_view_canvas_set_anim_policy for pacing and
 * refresh behavior. No capability required.
 * \{
 */

/// Maximum concurrently live tweens per canvas.
#define HOST_ANIM_MAX            16
/// `repeat` value meaning "repeat forever".
#define HOST_ANIM_REPEAT_FOREVER 0xFFFF

/* Easing curves (fixed-point Penner subset). */
#define HOST_EASE_LINEAR        0
#define HOST_EASE_QUAD_IN       1
#define HOST_EASE_QUAD_OUT      2
#define HOST_EASE_QUAD_IN_OUT   3
#define HOST_EASE_CUBIC_IN      4
#define HOST_EASE_CUBIC_OUT     5
#define HOST_EASE_CUBIC_IN_OUT  6
#define HOST_EASE_OVERSHOOT     7  /* back-out: overshoots, then settles  */
#define HOST_EASE_BOUNCE        8  /* bounce-out: ball-drop rebound       */
#define HOST_EASE_STEP          9  /* holds the start, jumps at the end   */
#define HOST_EASE_ELASTIC      10  /* elastic-out: springy overshoot wobble */

/* Tween flags. */
#define HOST_ANIM_FLAG_FROM_CURRENT 0x01 /* ignore from_x/y, start at the
                                            element's live offset          */
#define HOST_ANIM_FLAG_YOYO         0x02 /* each repeat alternates direction */
#define HOST_ANIM_FLAG_HIDE_DONE    0x04 /* hide the element on completion   */
#define HOST_ANIM_FLAG_SHOW_START   0x08 /* show the element when the delay
                                            elapses                        */

/** \brief Tween description for host_anim_start(); zero-init unused fields. */
typedef struct {
    uint32_t elem_id;        /**< Target element (must exist).                */
    int16_t  from_x;         /**< Start offset (see FLAG_FROM_CURRENT).       */
    int16_t  from_y;
    int16_t  to_x;           /**< End offset.                                 */
    int16_t  to_y;
    uint16_t duration_ms;    /**< Active time per run, 1..60000.              */
    uint16_t delay_ms;       /**< Wait before the first run, 0..60000.        */
    uint16_t repeat;         /**< Extra runs: 0 = play once,
                                  HOST_ANIM_REPEAT_FOREVER = endless.         */
    uint8_t  easing;         /**< HOST_EASE_*.                                */
    uint8_t  flags;          /**< HOST_ANIM_FLAG_*.                           */
    uint32_t done_action_id; /**< plugin_on_action(id, handle, elem_id) when a
                                  finite tween completes; 0 = none.           */
    uint32_t start_after;    /**< Live tween handle to chain behind; 0 = start
                                  immediately. Cancelling the predecessor
                                  cancels the whole chain.                    */
} host_anim_t;

/**
 * \brief Start (or queue, when `start_after` != 0) a tween on an element.
 * \return Tween handle >= 1, HOST_ERR_NOT_FOUND (element/predecessor/canvas),
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY (all slots live).
 */
int host_anim_start(const host_anim_t* cfg);

/**
 * \brief Cancel a tween and its chained successors; handle 0 cancels all of
 *        the canvas's tweens. Elements keep their current offset; no
 *        completion actions fire.
 * \return HOST_OK, HOST_ERR_NOT_FOUND.
 */
int host_anim_cancel(uint32_t handle);

/// \brief Pause (paused != 0) or resume a tween; delay time freezes too.
int host_anim_pause(uint32_t handle, bool paused);

/* Tween states returned by host_anim_state(). */
#define HOST_ANIM_STATE_DELAYED 0
#define HOST_ANIM_STATE_RUNNING 1
#define HOST_ANIM_STATE_PAUSED  2

/**
 * \brief Query a tween's state.
 * \return HOST_ANIM_STATE_*, or HOST_ERR_NOT_FOUND once completed/cancelled
 *         (handles are recycled after completion).
 */
int host_anim_state(uint32_t handle);

/// \brief Number of live tweens plus playing sprites (>= 0).
int host_anim_active_count(void);

/**
 * \brief Convenience: blink an element.
 *
 * Toggles visibility every `period_ms` for `count` on/off cycles
 * (HOST_ANIM_REPEAT_FOREVER = endless), ending visible. Occupies one tween
 * slot; `done_action_id` fires as in \ref host_anim_start.
 * \return Blink handle >= 1 (cancel/pause like a tween), HOST_ERR_NOT_FOUND,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY.
 */
int host_anim_blink(uint32_t elem_id, uint16_t period_ms, uint16_t count,
                    uint32_t done_action_id);

/** \} */

/**
 * \defgroup ui_lowlevel UI - Low-Level GFX
 * \brief Direct framebuffer drawing for advanced plugins.
 *
 * Opt-in via the manifest capability "display_lowlevel". Bypasses the view
 * system entirely; call \ref host_display_flush to push pixels to the panel.
 * \{
 */

/// \brief Display width in pixels.
uint16_t host_display_width    (void);

/// \brief Display height in pixels.
uint16_t host_display_height   (void);

/// \brief Clear the framebuffer to background.
int      host_display_clear    (void);

/// \brief Set a single pixel.
int      host_display_draw_pixel(int16_t x, int16_t y, uint16_t color);

/// \brief Draw a line between two points.
int      host_display_draw_line (int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color);

/// \brief Draw a rectangle outline.
int      host_display_draw_rect (int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/// \brief Draw a filled rectangle.
int      host_display_fill_rect (int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/// \brief Draw text using the default GFX font.
int      host_display_draw_text (int16_t x, int16_t y, const char* text, uint8_t size,
                                 uint16_t color);

/// \brief Push the framebuffer to the panel using the given refresh mode.
/// \param refresh_mode 0 = full (multi-flash, cleans all ghosting), 1 = partial
///        (no flash, may ghost), 2 = fast (single flash, clears most ghosting).
///        Unknown values behave like 0.
int      host_display_flush     (uint8_t refresh_mode);

/// \brief True while the panel is processing a previous refresh.
bool     host_display_is_busy   (void);

/// \brief Set the display backlight level (0 = off .. panel maximum, e.g. 1023).
///        Applied live; NOT persisted to NVS and NOT gated on display_lowlevel.
/// \return HOST_OK, or HOST_ERR_GENERIC if no display is available.
int      host_display_set_backlight(uint16_t level);

/// \brief Current backlight level (0 when no display is available).
uint16_t host_display_get_backlight(void);

/** \} */

/**
 * \defgroup i18n I18n
 * \brief Look up translated strings.
 *
 * Plugins ship per-language tables in their manifest; \ref host_i18n_tr_key
 * resolves keys against the plugin table, and \ref host_i18n_tr_core
 * resolves against the firmware's core string table.
 * \{
 */

#define HOST_LANG_EN 0
#define HOST_LANG_DE 1

/**
 * \brief Translate a plugin-local key into the current language.
 * \param out_cap Capacity of `out`; the result is NUL-terminated.
 */
int      host_i18n_tr_key          (const char* key,   char* out, uint32_t out_cap);

/// \brief Read a metadata field (name, description, ...) from the plugin manifest.
int      host_i18n_tr_meta         (const char* field, char* out, uint32_t out_cap);

/// \brief Translate a `core.*` key from the firmware string table.
int      host_i18n_tr_core         (const char* key,   char* out, uint32_t out_cap);

/// \brief Active language code (HOST_LANG_*).
uint8_t  host_i18n_current_language(void);

/** \} */

/**
 * \defgroup events EventBus
 * \brief Subscribe to system events and publish module events.
 *
 * Subscriptions are dispatched as plugin actions. Background-capable
 * plugins receive events even when not on screen. The action fires with
 * idx = the event-type ordinal and user_data = the event payload. Each
 * EVENT_* flag is defined as (1u << ordinal), where the ordinal is the value
 * of the matching cdc::core::EventType enumerator - that enum is the single
 * source of truth (a static_assert in host_api_event.cpp guards the match).
 * Plugin-facing events sit first in that enum, so these bits are contiguous.
 * For key events user_data is the ASCII key code ('0'..'9', 'Y' = 89, 'N' = 78).
 * \{
 */

#define EVENT_KEY_PRESSED       (1u <<  0)
#define EVENT_KEY_RELEASED      (1u <<  1)
#define EVENT_KEY_LONG_PRESS    (1u <<  2)
#define EVENT_POWER_USB_CONN    (1u <<  3)
#define EVENT_POWER_USB_DISCONN (1u <<  4)
#define EVENT_POWER_CHARGING    (1u <<  5)
#define EVENT_POWER_BATT_LOW    (1u <<  6)
#define EVENT_POWER_BATT_CRIT   (1u <<  7)
#define EVENT_SYSTEM_UNLOCK     (1u <<  8)
#define EVENT_SYSTEM_LOCK       (1u <<  9)
#define EVENT_SYSTEM_SLEEP      (1u << 10)
#define EVENT_SYSTEM_WAKE       (1u << 11)
#define EVENT_BLE_CONNECTED     (1u << 12)
#define EVENT_BLE_DISCONNECTED  (1u << 13)
#define EVENT_TIMER_TICK        (1u << 14)
#define EVENT_MODULE_EVENT      (1u << 15)
/// A FAST or FULL e-paper refresh is in progress: user_data = 1 at begin,
/// 0 at end. Subscribe to pause animation/game logic while the panel is
/// unreadable.
#define EVENT_DISPLAY_REFRESH   (1u << 16)

/**
 * \brief Subscribe to one or more events.
 * \param event_mask Bitwise OR of EVENT_* flags.
 * \param action_id Plugin action invoked when any subscribed event fires.
 * \return Subscription id > 0, or negative HostError.
 */
int host_event_subscribe  (uint32_t event_mask, uint32_t action_id);

/// \brief Cancel a subscription returned by host_event_subscribe.
int host_event_unsubscribe(uint32_t subscription_id);

/// \brief Publish an EVENT_MODULE_EVENT carrying `subtype` and `value`.
int host_event_publish    (uint32_t module_event_subtype, uint32_t value);

/** \} */

/**
 * \defgroup keypad Keypad
 * \brief Direct polling of the 12-button keypad.
 * \{
 */

#define KEY_0 0
#define KEY_1 1
#define KEY_2 2
#define KEY_3 3
#define KEY_4 4
#define KEY_5 5
#define KEY_6 6
#define KEY_7 7
#define KEY_8 8
#define KEY_9 9
#define KEY_Y 10
#define KEY_N 11

/// \brief True while `key` is currently held down.
bool host_key_pressed     (uint8_t key);

/**
 * \brief Pop the next queued key press, if any.
 * \param out_key Receives the KEY_* code on success.
 * \return HOST_OK when a key was returned, HOST_ERR_NOT_FOUND when the queue was empty.
 */
int  host_key_consume_next(uint8_t* out_key);

/** \} */

/**
 * \defgroup usb_cdc USB CDC
 * \brief Write raw bytes to the USB CDC serial endpoint.
 * \{
 */

/// \brief Write raw bytes to the USB-CDC TX stream.
int host_usb_cdc_write(const uint8_t* data, size_t len);

/** \} */

/**
 * \defgroup sysinfo System Info
 * \brief Query firmware identity and feature gating.
 * \{
 */

/// \brief True when the firmware was built with the given feature id enabled.
bool host_feature_enabled       (uint16_t feature_id);

/// \brief Copy the firmware semver string into `out`.
int  host_get_firmware_version  (char* out, size_t out_size);

/// \brief Copy the build profile name (e.g. "release", "debug") into `out`.
int  host_get_build_profile     (char* out, size_t out_size);

/// \brief Aggregate CPU load across all cores as 0..100 percent.
///        Sampled on demand from FreeRTOS run-time stats and refreshed at most
///        a few times per second; intermediate calls return the cached value.
///        The first call after load returns 0 (no baseline yet).
uint8_t host_cpu_load           (void);

/** \} */

/**
 * \defgroup cmd Plugin command channel
 * \brief Receive a command string pushed to the plugin by the host.
 *
 * When the host forwards a command (e.g. via the `PLUGIN CMD <id> <args>`
 * serial subcommand) it fires the optional \ref plugin_on_cmd export with the
 * command length. The plugin pulls the bytes into its own buffer with
 * \ref host_cmd_consume, mirroring \ref host_ui_consume_input_text.
 * \{
 */

/// \brief Copy the pending command string into `out`, clearing it.
/// \param out Caller buffer in plugin linear memory.
/// \param out_size Size of `out`; the result is always null-terminated.
/// \return Number of bytes copied, or a negative HOST_ERR_* code.
int host_cmd_consume (char* out, size_t out_size);

/** \} */

/**
 * \defgroup msg Message transfer (badge-to-badge)
 * \brief Register typed-payload handlers and push payloads to nearby badges.
 *
 * A plugin registers one or more MIME types it can receive. The firmware's
 * MessageTransfer service auto-declines an incoming BLE OFFER whose MIME type
 * has no registered handler. After the local user consents and the encrypted
 * transfer completes, the firmware fires the plugin's `action_id` on the plugin
 * tick task (deferred, like the BLE consume idiom); the handler then pulls the
 * payload with \ref host_msg_consume.
 *
 * To send, a plugin hands a typed payload to \ref host_msg_send_interactive,
 * which opens the firmware-owned peer picker and consent/progress UI and
 * returns immediately. Sending requires the manifest capability "ble" plus at
 * least one declared `message_types` entry.
 *
 * Payload bytes are opaque: the firmware does NOT convert them. For text MIME
 * types the bytes are UTF-8; the plugin renders them through the normal UI
 * functions, which convert UTF-8 to the display codepage. MIME type strings
 * are ASCII.
 * \{
 */

/// Maximum payload a plugin may send or receive in one transfer.
#define HOST_MSG_PAYLOAD_MAX 4096
/// Maximum MIME type string length including the NUL.
#define HOST_MSG_MIME_MAX 64

/**
 * \brief Send flag: remember the verified pairing for this runtime session.
 *
 * The first send still shows the numeric-comparison (and the peer's consent)
 * prompt once; afterwards follow-up sends to the same peer reconnect silently
 * with no prompt on either side. The trust is held in RAM only: it is dropped
 * on reboot and at clean teardown. Use for repeated traffic to the same peer
 * (e.g. a messenger); omit for one-shot sends.
 */
#define HOST_MSG_FLAG_PERSIST 0x01

/**
 * \brief Register that this plugin handles an incoming MIME type.
 *
 * Adds `mime_type` to the firmware MessageTransfer registry so an OFFER of that
 * type is no longer auto-declined. On a completed inbound transfer of this type
 * the firmware fires `plugin_on_action(action_id, 0, len)`; the handler reads
 * the bytes with \ref host_msg_consume. Re-registering the same MIME type
 * replaces the action id.
 * \param mime_type NUL-terminated ASCII MIME type, max HOST_MSG_MIME_MAX-1 bytes.
 * \param action_id Plugin action fired on a completed inbound transfer.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY, HOST_ERR_NO_MEMORY.
 */
int host_msg_register_handler(const char* mime_type, uint32_t action_id);

/**
 * \brief Drop a previously registered handler.
 * \param mime_type The MIME type passed to \ref host_msg_register_handler.
 * \return HOST_OK or HOST_ERR_NOT_FOUND.
 */
int host_msg_unregister_handler(const char* mime_type);

/**
 * \brief Pull the payload delivered by the most recent inbound message action.
 *
 * Call from the handler fired by \ref host_msg_register_handler. Copies the
 * received bytes into `buf` and the delivering MIME type into `mime_out`.
 * \param buf Destination for payload bytes.
 * \param buf_size Capacity of `buf`.
 * \param mime_out Destination for the NUL-terminated MIME type (may be NULL).
 * \param mime_size Capacity of `mime_out`.
 * \return Bytes copied into `buf` (>= 0), or a negative HOST_ERR_* code.
 */
int host_msg_consume(uint8_t* buf, size_t buf_size, char* mime_out, size_t mime_size);

/**
 * \brief Send a typed payload via the firmware-owned interactive peer picker.
 *
 * Opens the scan/peer-select UI, then the consent + progress flow, then
 * delivers `data` to the chosen peer's handler for `mime_type`. Returns
 * immediately after the picker is shown; the transfer happens asynchronously.
 * \param mime_type NUL-terminated ASCII MIME type, max HOST_MSG_MIME_MAX-1 bytes.
 * \param data Payload bytes, at most HOST_MSG_PAYLOAD_MAX.
 * \param len Number of payload bytes.
 * \param flags Bitwise OR of HOST_MSG_FLAG_* (0 for the default behaviour).
 * \return HOST_OK once the picker is shown, HOST_ERR_INVALID_ARG,
 *         HOST_ERR_NO_CAPABILITY, HOST_ERR_BUSY.
 */
int host_msg_send_interactive(const char* mime_type, const uint8_t* data, size_t len,
                              uint32_t flags);

/**
 * \brief Send a typed payload directly to a known peer address (no picker).
 *
 * The peer's user still consents. Use when the plugin already has an address;
 * most plugins prefer \ref host_msg_send_interactive.
 * \param addr 6-byte BLE address.
 * \param addr_type 0 = public, 1 = random.
 * \param mime_type NUL-terminated ASCII MIME type.
 * \param data Payload bytes, at most HOST_MSG_PAYLOAD_MAX.
 * \param len Number of payload bytes.
 * \param flags Bitwise OR of HOST_MSG_FLAG_* (0 for the default behaviour).
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY, HOST_ERR_BUSY.
 */
int host_msg_send(const uint8_t addr[6], uint8_t addr_type, const char* mime_type,
                  const uint8_t* data, size_t len, uint32_t flags);

/** \} */

/**
 * \defgroup ext_feature External features (inter-plugin)
 * \brief Invoke a named feature provided by another installed plugin.
 *
 * A provider plugin declares `provides: ["thermo_print"]` under manifest
 * capabilities and registers a handler for each declared feature in
 * `plugin_init` via \ref host_ext_feature_register_handler. A caller invokes
 * the feature with \ref host_ext_feature_use: the firmware stashes the payload,
 * switches the provider plugin to the FOREGROUND (like "Open with"), fires the
 * provider's handler action, and the provider pulls the bytes with
 * \ref host_ext_feature_consume. When done, the provider reports the outcome
 * once via \ref host_ext_feature_result, which fires the caller's
 * `status_action_id` with the status code in `user_data`.
 *
 * Lifecycle: HOST_OK from host_ext_feature_use means *job accepted*, not done.
 * The foreground switch unloads a caller without `background: true`
 * (plugin_on_exit + teardown) - its status action is then silently dropped.
 * Callers that need the result must declare `background: true`. Calling a
 * feature requires no capability; providing one requires the manifest
 * `provides` entry. If no installed plugin provides the feature, the firmware
 * shows a "no plugin provides this feature" modal and returns
 * HOST_ERR_NOT_FOUND.
 * \{
 */

/// Maximum external-feature name length including the NUL.
#define HOST_EXT_FEATURE_NAME_MAX 33
/// Maximum payload a caller may hand to a provider in one job.
#define HOST_EXT_FEATURE_PAYLOAD_MAX 32768
/// Provider status: job completed successfully.
#define HOST_EXT_FEATURE_STATUS_DONE 0
/// Provider status: generic failure. Provider-defined codes are >= 1.
#define HOST_EXT_FEATURE_STATUS_ERROR 1

/**
 * \brief Check whether an installed plugin provides `feature`.
 * \param feature NUL-terminated feature name, [a-z][a-z0-9_]*.
 * \return HOST_OK if a provider is installed (and enabled),
 *         HOST_ERR_NOT_FOUND otherwise, HOST_ERR_INVALID_ARG on a bad name.
 */
int host_ext_feature_available(const char* feature);

/**
 * \brief Invoke `feature` with a payload; the provider runs in the foreground.
 *
 * Returns immediately after stashing the job. The provider plugin is switched
 * to the foreground on the next tick, receives its registered handler action
 * and consumes the payload. The final outcome arrives later as
 * `plugin_on_action(status_action_id, 0, status)` - only if this caller is
 * still loaded at that time (see group docs for the background requirement).
 * \param feature NUL-terminated feature name.
 * \param data Payload bytes (may be NULL when len == 0).
 * \param len Payload length, at most HOST_EXT_FEATURE_PAYLOAD_MAX.
 * \param status_action_id Caller action fired with the provider's status code
 *        in `user_data` (HOST_EXT_FEATURE_STATUS_*). Pass 0 for fire-and-forget.
 * \return HOST_OK (job accepted), HOST_ERR_INVALID_ARG (bad name/args or
 *         self-call), HOST_ERR_NOT_FOUND (no provider installed - a modal was
 *         shown), HOST_ERR_BUSY (another job is still pending),
 *         HOST_ERR_NO_MEMORY.
 */
int host_ext_feature_use(const char* feature, const uint8_t* data, size_t len,
                         uint32_t status_action_id);

/**
 * \brief Register this plugin as the live handler for a feature it provides.
 *
 * Call from `plugin_init` for every manifest `provides` entry. On an incoming
 * job the firmware fires `plugin_on_action(action_id, 0, len)`; the handler
 * pulls the payload with \ref host_ext_feature_consume. The handler firing
 * right after plugin_on_enter is what distinguishes "opened for a job" from
 * "opened by the user".
 * \param feature NUL-terminated feature name; must be declared in `provides`.
 * \param action_id Plugin action fired when a job arrives.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY (feature not
 *         in this plugin's `provides`), HOST_ERR_NO_MEMORY.
 */
int host_ext_feature_register_handler(const char* feature, uint32_t action_id);

/**
 * \brief Pull the payload of the job that fired the current handler action.
 *
 * Valid only during that action dispatch, mirroring \ref host_msg_consume.
 * \param buf Destination for payload bytes.
 * \param buf_size Capacity of `buf`.
 * \param feature_out Destination for the NUL-terminated feature name (may be NULL).
 * \param feature_size Capacity of `feature_out`.
 * \return Bytes copied into `buf` (>= 0), or a negative HOST_ERR_* code.
 */
int host_ext_feature_consume(uint8_t* buf, size_t buf_size,
                             char* feature_out, size_t feature_size);

/**
 * \brief Report the outcome of the job this provider is currently handling.
 *
 * Fires the caller's `status_action_id` (if the caller is still loaded) and
 * frees the job slot. Call exactly once per received job.
 * \param status_code HOST_EXT_FEATURE_STATUS_DONE or a provider-defined
 *        error code >= 1.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (no job owned by this plugin).
 */
int host_ext_feature_result(int32_t status_code);

/** \} */

/**
 * \defgroup vcard vCard store
 * \brief Read and manage the badge's own vCard and received vCards.
 *
 * Requires the manifest capability `vcard: true`. All strings are vCard 4.0
 * UTF-8 text as stored; display strings are the parsed formatted names used
 * by the firmware's own contact list. Received cards are addressed by their
 * sorted position (the order the firmware lists them in), 0-based; write
 * operations re-sort, so re-read positions after any mutation.
 * \{
 */

/// Maximum length of a single vCard text including the NUL.
#define HOST_VCARD_MAX_LEN 768

/**
 * \brief Copy the badge's own vCard text into `out`.
 * \param out Caller buffer; the result is NUL-terminated.
 * \param out_size Capacity of `out` (HOST_VCARD_MAX_LEN + 1 always fits).
 * \return Bytes copied (> 0), HOST_ERR_NOT_FOUND when no own card is set,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_get_own(char* out, size_t out_size);

/**
 * \brief Number of received vCards in the store.
 * \return Count >= 0, or HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_count(void);

/**
 * \brief Copy the received vCard at sorted position `index` into `out`.
 * \param index 0-based sorted position, < host_vcard_received_count().
 * \param out Caller buffer; the result is NUL-terminated.
 * \param out_size Capacity of `out`.
 * \return Bytes copied (> 0), HOST_ERR_NOT_FOUND on a bad index,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_get(uint16_t index, char* out, size_t out_size);

/**
 * \brief Copy the display name of the received vCard at sorted position
 *        `index` into `out` (for list rows).
 * \return Bytes copied (> 0), HOST_ERR_NOT_FOUND on a bad index,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_display(uint16_t index, char* out, size_t out_size);

/**
 * \brief Set / replace the badge's own vCard.
 * \param vcard vCard 4.0 UTF-8 text, at most HOST_VCARD_MAX_LEN - 1 bytes.
 * \param len Text length.
 * \return HOST_OK, HOST_ERR_INVALID_ARG (validation failed - see log),
 *         HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_set_own(const char* vcard, size_t len);

/**
 * \brief Store a new received vCard.
 * \param vcard vCard 4.0 UTF-8 text, at most HOST_VCARD_MAX_LEN - 1 bytes.
 * \param len Text length.
 * \return HOST_OK, HOST_ERR_INVALID_ARG (validation failed),
 *         HOST_ERR_BUSY (duplicate already stored), HOST_ERR_NO_MEMORY
 *         (store full), HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_add(const char* vcard, size_t len);

/**
 * \brief Overwrite the received vCard at sorted position `index`.
 * \return HOST_OK, HOST_ERR_NOT_FOUND on a bad index, HOST_ERR_INVALID_ARG
 *         (validation failed), HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_update(uint16_t index, const char* vcard, size_t len);

/**
 * \brief Delete the received vCard at sorted position `index`.
 * \return HOST_OK, HOST_ERR_NOT_FOUND on a bad index, HOST_ERR_NO_CAPABILITY.
 */
int host_vcard_received_delete(uint16_t index);

/** \} */

/**
 * \defgroup qr QR encoding
 * \brief Encode text into a 1-bpp packed QR raster (no capability required).
 *
 * Pure compute on caller memory. The output layout matches the surface and
 * image APIs: packed rows, MSB-first, a set bit is black. Blit the result
 * onto a surface with host_surface_draw_bitmap or send it to a printer.
 * \{
 */

/**
 * \brief Measure the QR module count for `data` without rendering.
 * \param data NUL-terminated payload text.
 * \param max_version Maximum QR version 1..20 (0 = 20).
 * \param ecc ECC level 0..3 (LOW, MED, QUART, HIGH).
 * \param out_modules Receives the side length in modules.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_GENERIC (data too long).
 */
int host_qr_measure(const char* data, uint8_t max_version, uint8_t ecc,
                    uint16_t* out_modules);

/**
 * \brief Encode `data` into a packed 1-bpp QR raster.
 *
 * Side length is `(modules + 2 * quiet_modules) * scale` pixels, capped at
 * 1024. The quiet zone renders white.
 * \param data NUL-terminated payload text.
 * \param max_version Maximum QR version 1..20 (0 = 20).
 * \param ecc ECC level 0..3.
 * \param scale Pixels per module (0 counts as 1).
 * \param quiet_modules Quiet-zone width in modules per edge.
 * \param out Caller buffer for packed rows.
 * \param out_size Capacity of `out` in bytes.
 * \param out_stride_bytes Receives the row stride in bytes.
 * \param out_height_px Receives the side length in pixels.
 * \return HOST_OK, HOST_ERR_INVALID_ARG (bad args or side > 1024),
 *         HOST_ERR_NO_MEMORY (`out` too small), HOST_ERR_GENERIC.
 */
int host_qr_render_bitmap(const char* data, uint8_t max_version, uint8_t ecc,
                          uint8_t scale, uint8_t quiet_modules,
                          uint8_t* out, size_t out_size,
                          uint16_t* out_stride_bytes, uint16_t* out_height_px);

/** \} */

/**
 * \defgroup image Image decoding
 * \brief Decode PNG/JPEG bytes into a dithered 1-bpp raster (no capability).
 *
 * Pure compute on caller memory. Output layout matches the QR and surface
 * APIs: packed rows, MSB-first, a set bit is black. Decoding uses the same
 * engine as the firmware image viewer (max ~1 megapixel input).
 * \{
 */

/**
 * \brief Decode only the image header to get its dimensions.
 * \param data Encoded PNG or JPEG bytes.
 * \param len Byte length.
 * \param out_w Receives the native width in pixels.
 * \param out_h Receives the native height in pixels.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_GENERIC (unsupported or
 *         corrupt image, too many pixels).
 */
int host_image_info(const uint8_t* data, size_t len, uint16_t* out_w, uint16_t* out_h);

/**
 * \brief Decode, scale to `target_w` (aspect preserved) and dither to 1-bpp.
 * \param data Encoded PNG or JPEG bytes.
 * \param len Byte length.
 * \param target_w Output width in pixels, 1..1024 (e.g. 384 for thermal print).
 * \param out Caller buffer for packed rows.
 * \param out_size Capacity of `out` in bytes.
 * \param out_stride_bytes Receives the row stride `(target_w + 7) / 8`.
 * \param out_height_px Receives the scaled height.
 * \return HOST_OK, HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY (`out` too small
 *         or allocation failed), HOST_ERR_GENERIC (decode failure).
 */
int host_image_render(const uint8_t* data, size_t len, uint16_t target_w,
                      uint8_t* out, size_t out_size,
                      uint16_t* out_stride_bytes, uint16_t* out_height_px);

/** \} */

/**
 * \defgroup surface Offscreen surfaces
 * \brief Compose 1-bpp images of arbitrary size with the canvas primitives.
 *
 * A surface is an offscreen render target independent of the view stack -
 * e.g. 384 px wide for a thermal print band while the display is only
 * 296x128. The draw API mirrors the canvas view (same fonts, shades and
 * primitives); coordinates are raw surface pixels with no header offset.
 * Export yields packed rows (MSB-first, set bit = black - the same layout
 * as the QR and image APIs) or an encoded grayscale JPEG.
 *
 * No capability required: pure compute with a hard memory cap. A plugin can
 * hold at most 2 surfaces of up to 64 KiB pixel data each; all surfaces are
 * freed when the plugin unloads.
 * \{
 */

/// Maximum surfaces a plugin may hold at once.
#define HOST_SURFACE_MAX_PER_PLUGIN 2
/// Maximum packed pixel bytes per surface (stride * height).
#define HOST_SURFACE_MAX_BYTES 65536

/**
 * \brief Create a `w x h` surface, cleared to white.
 * \param w Width in pixels, 1..1024.
 * \param h Height in pixels, 1..2048; `((w+7)/8) * h` <= HOST_SURFACE_MAX_BYTES.
 * \return Surface handle >= 1, HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY
 *         (allocation failed or too many surfaces).
 */
int host_surface_create(uint16_t w, uint16_t h);

/// \brief Destroy a surface and free its memory.
int host_surface_destroy(uint32_t surface);

/// \brief Clear a surface to white.
int host_surface_clear(uint32_t surface);

/// \brief Select the font for subsequent text calls (HOST_FONT_* id).
int host_surface_set_font(uint32_t surface, uint8_t font_id);

/// \brief Set the integer text scale (1..4) for subsequent text calls.
int host_surface_set_text_size(uint32_t surface, uint8_t size);

/// \brief Draw text white-on-black (inverted != 0) or black-on-white.
int host_surface_set_text_color(uint32_t surface, uint8_t inverted);

/// \brief Set the fill shade for filled shapes: 0 white .. 255 solid black.
int host_surface_set_shade(uint32_t surface, uint8_t shade);

/// \brief Draw UTF-8 text with the current font/size at (x, y) baseline-top.
int host_surface_draw_text(uint32_t surface, int16_t x, int16_t y, const char* text);

/**
 * \brief Draw UTF-8 text aligned within a `w`-wide box starting at x.
 * \param align 0 = left, 1 = center, 2 = right.
 */
int host_surface_draw_text_aligned(uint32_t surface, int16_t x, int16_t y, int16_t w,
                                   const char* text, uint8_t align);

/**
 * \brief Measure UTF-8 text with the surface's current font/size.
 * \param out_w Receives the rendered width in pixels.
 * \param out_h Receives the rendered height in pixels.
 */
int host_surface_measure_text(uint32_t surface, const char* text,
                              uint16_t* out_w, uint16_t* out_h);

/// \brief Draw a pixel (current shade decides ink: shade > 0 draws black).
int host_surface_draw_pixel(uint32_t surface, int16_t x, int16_t y);

/// \brief Draw a line.
int host_surface_draw_line(uint32_t surface, int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/// \brief Draw a rectangle; filled ones use the current shade.
int host_surface_draw_rect(uint32_t surface, int16_t x, int16_t y, int16_t w, int16_t h,
                           uint8_t filled);

/// \brief Draw a circle centered at (x, y); filled ones use the current shade.
int host_surface_draw_circle(uint32_t surface, int16_t x, int16_t y, int16_t r,
                             uint8_t filled);

/// \brief Draw a triangle; filled ones use the current shade.
int host_surface_draw_triangle(uint32_t surface, int16_t x0, int16_t y0,
                               int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                               uint8_t filled);

/// \brief Draw a rounded rectangle with corner radius r.
int host_surface_draw_round_rect(uint32_t surface, int16_t x, int16_t y,
                                 int16_t w, int16_t h, int16_t r, uint8_t filled);

/// \brief Draw a horizontal line of width w.
int host_surface_hline(uint32_t surface, int16_t x, int16_t y, int16_t w);

/// \brief Draw a vertical line of height h.
int host_surface_vline(uint32_t surface, int16_t x, int16_t y, int16_t h);

/**
 * \brief Blit a packed 1-bpp bitmap (MSB-first, set bit = black) at (x, y).
 * \param len Must be at least `((w + 7) / 8) * h`.
 */
int host_surface_draw_bitmap(uint32_t surface, int16_t x, int16_t y,
                             int16_t w, int16_t h, const uint8_t* data, uint32_t len);

/**
 * \brief Copy the surface's packed pixel rows into `out`.
 * \param out_stride_bytes Receives the row stride `(width + 7) / 8`.
 * \return Bytes copied (> 0), HOST_ERR_NO_MEMORY when `out` is too small,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NOT_FOUND.
 */
int host_surface_export(uint32_t surface, uint8_t* out, size_t out_size,
                        uint16_t* out_stride_bytes);

/**
 * \brief Encode the surface as a grayscale JPEG.
 * \param quality 1..100 (0 = 85).
 * \param out_len Receives the encoded size; on HOST_ERR_NO_MEMORY it holds
 *        the required buffer size instead.
 * \return HOST_OK, HOST_ERR_NO_MEMORY (`out` too small), HOST_ERR_GENERIC,
 *         HOST_ERR_INVALID_ARG, HOST_ERR_NOT_FOUND.
 */
int host_surface_export_jpg(uint32_t surface, uint8_t quality,
                            uint8_t* out, size_t out_size, uint32_t* out_len);

/**
 * \brief Stamp a sprite's current frame onto a surface at (x, y).
 *
 * Composition helper: honors the sprite's mask and flip flags. Unlike
 * \ref host_view_canvas_draw_sprite this is an immediate pixel copy, not a
 * reference.
 * \return HOST_OK, HOST_ERR_NOT_FOUND (unknown surface/sprite).
 */
int host_surface_draw_sprite(uint32_t surface, int16_t x, int16_t y, uint32_t sprite);

/** \} */

/**
 * \defgroup sprites Sprites
 * \brief Multi-frame 1-bpp resources with host-driven frame playback.
 *
 * A sprite is a frame sheet: `frame_count` frames of `w x h` pixels stacked
 * vertically, packed rows MSB-first, set bit = black - the same layout as the
 * surface, QR and image APIs, so \ref host_image_render output slices
 * straight into frames. Draw it into the canvas by reference with
 * \ref host_view_canvas_draw_sprite (inside an element for movement/z/tweens)
 * and let \ref host_sprite_play advance frames on the host clock.
 *
 * Transparency: by default set bits paint black and unset bits are
 * transparent. HOST_SPRITE_FLAG_OPAQUE paints unset bits white. A mask plane
 * (\ref host_sprite_set_mask) gives per-pixel control: mask bit set = pixel
 * painted (black or white per the data bit), unset = transparent.
 *
 * Sprites live in the canvas: they are freed when the canvas is popped or
 * cleared, and creating them requires an open canvas view. To create sheets
 * once and reuse them across screen rebuilds, clear with
 * \ref host_view_canvas_clear_ex (HOST_CANVAS_CLEAR_KEEP_SPRITES) - assets
 * survive, playback stops. No capability required - pure compute with a
 * hard memory cap.
 * \{
 */

/// Maximum sprites per canvas.
#define HOST_SPRITE_MAX_PER_CANVAS 8
/// Maximum frames per sprite.
#define HOST_SPRITE_MAX_FRAMES     32
/// Shared pixel arena for all sprite data (frames + masks + durations).
#define HOST_SPRITE_ARENA_BYTES    65536

/* Playback modes. */
#define HOST_SPRITE_ONCE      0  /* play to the last frame and hold it     */
#define HOST_SPRITE_LOOP      1  /* wrap around; finite runs hold the last */
#define HOST_SPRITE_PING_PONG 2  /* forward then backward per cycle        */

/* Sprite flags. */
#define HOST_SPRITE_FLAG_OPAQUE 0x01 /* unset data bits paint white         */
#define HOST_SPRITE_FLAG_FLIP_H 0x02 /* mirror horizontally when drawn      */
#define HOST_SPRITE_FLAG_FLIP_V 0x04 /* mirror vertically when drawn        */
#define HOST_SPRITE_FLAG_ROT_90 0x08 /* rotate 90 deg clockwise (before the
                                        flips; combine for 180/270). The
                                        drawn box becomes h x w.            */

/**
 * \brief Create a sprite from a packed 1-bpp frame sheet in plugin memory.
 *
 * The data is copied into the host's sprite arena; the buffer may be reused.
 * \param frame_w Frame width in pixels, >= 1.
 * \param frame_h Frame height in pixels, >= 1.
 * \param frame_count 1..HOST_SPRITE_MAX_FRAMES.
 * \param frames Sheet bytes, frames stacked vertically.
 * \param len Must be >= `((frame_w + 7) / 8) * frame_h * frame_count`.
 * \return Sprite handle >= 1, HOST_ERR_INVALID_ARG, HOST_ERR_NO_MEMORY
 *         (slots or arena exhausted), HOST_ERR_NOT_FOUND (no canvas).
 */
int host_sprite_create(uint16_t frame_w, uint16_t frame_h, uint16_t frame_count,
                       const uint8_t* frames, uint32_t len);

/**
 * \brief Create a sprite by slicing a surface into a row-major grid of
 *        `frame_w x frame_h` cells.
 *
 * Compose frames with text, shapes or decoded PNGs on a surface first, then
 * snapshot it. Pixels are copied; the surface may be destroyed afterwards.
 * \param frame_count Cells taken from the grid, left-to-right, top-to-bottom.
 * \return Sprite handle >= 1, HOST_ERR_NOT_FOUND (surface/canvas),
 *         HOST_ERR_INVALID_ARG (grid does not fit), HOST_ERR_NO_MEMORY.
 */
int host_sprite_create_from_surface(uint32_t surface, uint16_t frame_w,
                                    uint16_t frame_h, uint16_t frame_count);

/**
 * \brief Attach a transparency mask plane (same sheet layout and size as the
 *        frame data). Mask bit set = pixel painted; overrides
 *        HOST_SPRITE_FLAG_OPAQUE.
 * \return HOST_OK, HOST_ERR_NOT_FOUND, HOST_ERR_INVALID_ARG (short buffer),
 *         HOST_ERR_NO_MEMORY.
 */
int host_sprite_set_mask(uint32_t sprite, const uint8_t* mask, uint32_t len);

/**
 * \brief Create a sprite straight from an encoded PNG/JPEG.
 *
 * The image is decoded, scaled to `target_w` (aspect preserved), dithered to
 * 1-bpp (the \ref host_image_render pipeline) and sliced vertically into
 * frames of `frame_h` pixels; a partial last frame is dropped. Compose the
 * source as one tall filmstrip.
 * \return Sprite handle >= 1, HOST_ERR_INVALID_ARG, HOST_ERR_GENERIC
 *         (decode failed), HOST_ERR_NO_MEMORY, HOST_ERR_NOT_FOUND (no canvas).
 */
int host_sprite_create_from_image(const uint8_t* data, uint32_t len,
                                  uint16_t target_w, uint16_t frame_h);

/// \brief Replace the sprite's flag set (HOST_SPRITE_FLAG_*).
int host_sprite_set_flags(uint32_t sprite, uint8_t flags);

/**
 * \brief Integer upscale factor applied whenever the sprite is drawn.
 * \param scale 1..4; the drawn box becomes `w*scale x h*scale`. Lossless on
 *        1-bpp - pixels just get fatter.
 */
int host_sprite_set_scale(uint32_t sprite, uint8_t scale);

/// \brief Jump to a frame. A running playback continues from it.
int host_sprite_set_frame(uint32_t sprite, uint16_t frame);

/// \brief Read the currently displayed frame index.
int host_sprite_get_frame(uint32_t sprite, uint16_t* out);

/**
 * \brief Optional per-frame durations in milliseconds.
 * \param count Must equal the sprite's frame count; 0 reverts to the global
 *        `frame_ms` given to \ref host_sprite_play.
 * \return HOST_OK, HOST_ERR_NOT_FOUND, HOST_ERR_INVALID_ARG,
 *         HOST_ERR_NO_MEMORY.
 */
int host_sprite_set_frame_durations(uint32_t sprite, const uint16_t* ms,
                                    uint16_t count);

/**
 * \brief Start host-driven playback from frame 0.
 *
 * The canvas is committed automatically while playback runs (see
 * \ref host_view_canvas_set_anim_policy).
 * \param mode HOST_SPRITE_ONCE / LOOP / PING_PONG.
 * \param frame_ms Per-frame duration, floored at 50 ms (overridden by
 *        \ref host_sprite_set_frame_durations).
 * \param repeat Extra cycles: 0 = one pass, HOST_ANIM_REPEAT_FOREVER =
 *        endless. A PING_PONG cycle is one full there-and-back.
 * \param done_action_id plugin_on_action(id, sprite, final_frame) when a
 *        finite playback completes; 0 = none.
 * \return HOST_OK, HOST_ERR_NOT_FOUND, HOST_ERR_INVALID_ARG.
 */
int host_sprite_play(uint32_t sprite, uint8_t mode, uint16_t frame_ms,
                     uint16_t repeat, uint32_t done_action_id);

/// \brief Stop playback, keeping the current frame on screen.
int host_sprite_stop(uint32_t sprite);

/**
 * \brief Destroy a sprite and reclaim its arena bytes. Recorded draw-sprite
 *        commands referencing it are skipped from then on.
 */
int host_sprite_destroy(uint32_t sprite);

/** \} */

/**
 * \defgroup strings Strings (explicit display normalisation)
 * \brief Optional UTF-8 <-> display-codepage conversion helpers.
 *
 * The whole host API speaks UTF-8: text passed to UI / canvas / display
 * functions is converted to the display codepage internally, and text read
 * back (host_ui_consume_input_text, host_view_canvas_get_text) is returned as
 * UTF-8. Plugins normally never need to convert anything themselves.
 *
 * These two helpers remain for advanced cases, e.g. pre-rendering a string to
 * a specific codepage. Do not feed their output back into the auto-converting
 * UI functions, or the text is encoded twice.
 * \{
 */

/// Target codepage for host_str_to_display().
#define HOST_STR_TARGET_CP437   0  /* GFX builtin glcdfont (default after splash) */
#define HOST_STR_TARGET_LATIN1  1  /* FreeMonoBold*pt8b fonts (Latin-1 indexed)   */

/// \brief Decode HTML entities + UTF-8 in `in` into single-byte display
///        characters in `out`.
///
/// Optional: the UI / canvas / display functions already perform this on the
/// text you pass them. `target` selects the output codepage. Unknown
/// codepoints are dropped. Truncates if the result would exceed `out_size - 1`
/// bytes. Output is always NUL-terminated.
/// \param in       Source string (UTF-8 with optional HTML entities).
/// \param out      Destination buffer.
/// \param out_size Capacity of `out` in bytes (including the NUL).
/// \param target   One of HOST_STR_TARGET_*.
/// \return HOST_OK on success, HOST_ERR_INVALID_ARG when inputs are NULL or `out_size==0`.
int host_str_to_display(const char* in, char* out, size_t out_size, uint32_t target);

/// \brief Convert CP437 display bytes in `in` to a UTF-8 string in `out`.
///
/// Optional: host_ui_consume_input_text and host_view_canvas_get_text already
/// return UTF-8.
/// \param in       Source CP437 string.
/// \param out      Destination buffer (always NUL-terminated, truncated to fit).
/// \param out_size Capacity of `out` in bytes (including the NUL).
/// \return Bytes written (excluding NUL), or a negative HOST_ERR_* code.
int host_str_to_utf8(const char* in, char* out, size_t out_size);

/** \} */

/**
 * \defgroup gpio Hardware: GPIO / PWM / ADC / I2C / SAO
 * \brief Direct access to user-accessible GPIO, ADC, I2C and the SAO EEPROM.
 *
 * Pin usage must be declared in the manifest (`capabilities.gpio_pins`,
 * `pwm_pins`, `adc_pins`). Conflicting claims fail with HOST_ERR_BUSY.
 * \{
 */

#define GPIO_DIR_IN     0
#define GPIO_DIR_OUT    1
#define GPIO_DIR_OUT_OD 2

#define GPIO_PULL_NONE  0
#define GPIO_PULL_UP    1
#define GPIO_PULL_DOWN  2

/// \brief Configure pin direction (one of GPIO_DIR_*).
int host_gpio_set_direction(uint8_t pin, uint8_t direction);

/// \brief Configure internal pull resistor (one of GPIO_PULL_*).
int host_gpio_set_pull     (uint8_t pin, uint8_t pull);

/// \brief Drive a digital output high/low.
int host_gpio_write        (uint8_t pin, bool level);

/// \brief Sample a digital input.
int host_gpio_read         (uint8_t pin, bool* level);

/// \brief Release the pin claim so other plugins can use it.
int host_gpio_release      (uint8_t pin);

/**
 * \brief Start LEDC PWM on `pin`.
 * \param duty_per_mille Duty cycle in 0..1000 (per-mille resolution).
 */
int host_gpio_pwm_start    (uint8_t pin, uint32_t freq_hz, uint16_t duty_per_mille);

/// \brief Update PWM duty without restarting the timer.
int host_gpio_pwm_set_duty (uint8_t pin, uint16_t duty_per_mille);

/// \brief Stop PWM and release the LEDC channel.
int host_gpio_pwm_stop     (uint8_t pin);

/**
 * \brief Single-shot ADC read.
 * \param raw Raw ADC count, or NULL to skip.
 * \param millivolt Calibrated voltage in mV, or NULL to skip.
 */
int host_adc_read          (uint8_t pin, uint16_t* raw, uint16_t* millivolt);

/// \brief I2C write transaction.
int host_i2c_write         (uint8_t bus, uint8_t addr, const uint8_t* data, size_t len);

/// \brief I2C read transaction.
int host_i2c_read          (uint8_t bus, uint8_t addr, uint8_t* data, size_t len);

/// \brief I2C write-then-read transaction with repeated start.
int host_i2c_write_read    (uint8_t bus, uint8_t addr,
                            const uint8_t* wr, size_t wr_len,
                            uint8_t* rd, size_t rd_len);

/**
 * \brief Scan the I2C bus for responding addresses.
 * \param count In: capacity of `found_addrs`; out: number of devices found.
 */
int host_i2c_scan          (uint8_t bus, uint8_t* found_addrs, size_t* count);

/// \brief Read from the SAO addon EEPROM at byte `offset`.
int host_sao_eeprom_read   (uint16_t offset, uint8_t* buf, size_t len);

/// \brief Write to the SAO addon EEPROM at byte `offset`.
int host_sao_eeprom_write  (uint16_t offset, const uint8_t* buf, size_t len);

/** \} */

/**
 * \defgroup pixel_strip Addressable pixel strip
 * \brief WS2811/WS2812/WS2813/SK6812 strip via RMT.
 *
 * The host owns one global strip handle keyed to the (gpio_pin, num_pixels,
 * format) tuple given to the first successful init. Re-init with the same
 * tuple is a no-op; with a different tuple the previous handle is replaced.
 * Requires manifest capability "pixel_strip".
 * \{
 */

#define PIXEL_FORMAT_GRB  0  /* WS2812/WS2813/SK6812 */
#define PIXEL_FORMAT_RGB  1
#define PIXEL_FORMAT_GRBW 2  /* SK6812 RGBW (white byte = 0 for plugin-side use) */
#define PIXEL_FORMAT_RGBW 3

/// \brief Initialise or reconfigure the global pixel strip.
int      host_pixel_strip_init    (uint8_t gpio_pin, uint16_t num_pixels, uint8_t format);

/// \brief Tear down the global pixel strip.
int      host_pixel_strip_deinit  (void);

/// \brief Set one pixel's RGB colour in the strip buffer.
int      host_pixel_strip_set     (uint16_t index, uint8_t r, uint8_t g, uint8_t b);

/// \brief Fill every pixel with the same RGB colour.
int      host_pixel_strip_fill    (uint8_t r, uint8_t g, uint8_t b);

/// \brief Clear every pixel to off (0, 0, 0).
int      host_pixel_strip_clear   (void);

/// \brief Push the strip buffer out over the RMT bus.
int      host_pixel_strip_refresh (void);

/// \brief Number of pixels the strip was initialised with.
uint16_t host_pixel_strip_length  (void);

/// \brief True when the strip has been successfully initialised.
bool     host_pixel_strip_ready   (void);

/** \} */

/**
 * \defgroup lockscreen Lockscreen quick-action slot
 * \brief Register a single lockscreen quick-action for background plugins.
 *
 * A plugin may register exactly one item that appears in the lockscreen
 * context menu (opened by KEY_MENU). When the user selects it, the plugin's
 * `plugin_on_action(action_id, 0, 0)` fires. The label is an i18n key
 * resolved per-language via \ref host_i18n_tr_key.
 * \{
 */

/// \brief Publish (or replace) the plugin's lockscreen quick-action.
int host_lockscreen_register_action  (const char* label_key, uint32_t action_id);

/// \brief Remove the plugin's lockscreen quick-action.
int host_lockscreen_unregister_action(void);

/// \brief Raise a persistent Y/N alert over whatever is on screen, lock screen
///        included, that stays until the user answers.
///
/// Intended for background plugins that need to reach the user while their view
/// is not in front. The alert overlays the current screen as a modal and does
/// not auto-dismiss. When the user answers, the originating plugin's
/// `plugin_on_action(action_id, 0, user_data)` fires with `user_data=1` for Y
/// (confirm) or `user_data=0` for N (cancel); the answer is routed to that plugin
/// even while it is running in the background. Only one alert can be pending at a time.
/// \param text      Message to display (UTF-8; HTML entities are decoded).
/// \param icon      One of UI_ICON_* (ERROR/ALERT pick a matching glyph).
/// \param action_id Action id echoed back to the plugin with the answer.
/// \return HOST_OK, HOST_ERR_INVALID_ARG for a NULL text, HOST_ERR_NO_CAPABILITY
///         without an active plugin, or HOST_ERR_BUSY when another modal or an
///         exclusive prompt is already on screen.
int host_lockscreen_alert            (const char* text, uint8_t icon, uint32_t action_id);

/** \} */

#ifdef __cplusplus
}
#endif

#endif /* CDC_BADGE_HOST_API_H */
