// Host-test shim for cdc_core/Raii.h: NvsScope backed by a tiny in-memory
// u8 key-value store so the UsbServiceManager persistence path can be tested
// without the ESP-IDF NVS.
#pragma once
#include <cstdint>
#include <cstring>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef uint32_t nvs_handle_t;

namespace nvs_fake {

struct Entry {
    char key[16];
    uint8_t value;
    bool used;
};

inline Entry g_entries[16] = {};
inline bool g_openFails = false;

inline Entry* find(const char* key) {
    for (auto& e : g_entries) {
        if (e.used && strcmp(e.key, key) == 0) return &e;
    }
    return nullptr;
}

inline void reset() {
    memset(g_entries, 0, sizeof(g_entries));
    g_openFails = false;
}

} // namespace nvs_fake

inline esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out) {
    const nvs_fake::Entry* e = nvs_fake::find(key);
    if (!e) return ESP_FAIL;
    *out = e->value;
    return ESP_OK;
}

inline esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t value) {
    nvs_fake::Entry* e = nvs_fake::find(key);
    if (!e) {
        for (auto& slot : nvs_fake::g_entries) {
            if (!slot.used) {
                slot.used = true;
                strncpy(slot.key, key, sizeof(slot.key) - 1);
                e = &slot;
                break;
            }
        }
        if (!e) return ESP_FAIL;
    }
    e->value = value;
    return ESP_OK;
}

class NvsScope {
public:
    NvsScope() = default;
    NvsScope(const char*, nvs_open_mode_t) : ok_(!nvs_fake::g_openFails) {}

    explicit operator bool() const { return ok_; }
    operator nvs_handle_t() const { return 1; }
    esp_err_t commit() { return ok_ ? ESP_OK : ESP_FAIL; }

private:
    bool ok_ = false;
};
