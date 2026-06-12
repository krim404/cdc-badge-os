/**
 * \file
 * \brief Encrypted, versioned, semantic backup loader.
 *
 * Owns the single on-device backup container: it collects each module's JSON
 * section via IModule::exportBackup, serializes one document, encrypts it with
 * AES-256-GCM under a PBKDF2-HMAC-SHA256 key derived from the export passphrase,
 * and writes only the ciphertext to the vFAT (plugins) partition. Restore
 * reverses this and routes each section back to its module best-effort.
 */

#include "cdc_os_ui/BackupManager.h"
#include "cdc_os_ui/SystemSettingsBackup.h"

#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/IModule.h"
#include "cdc_core/Raii.h"
#include "cdc_core/Crypto.h"
#include "cdc_ui/PsramCjson.h"
#include "cdc_log.h"
#include "plugin_manager/PluginStorage.h"
#include "plugin_manager/host_api.h"

#include "cJSON.h"
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/base64.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace cdc::os_ui {

namespace {

constexpr const char* TAG = "Backup";

/// Container layout constants (see BackupManager.h for the on-disk format).
constexpr uint8_t MAGIC[] = {'C', 'D', 'C', 'B', 'A', 'K'};
constexpr size_t MAGIC_SIZE = sizeof(MAGIC);
constexpr uint8_t CONTAINER_VERSION = 1;
constexpr size_t SALT_SIZE = 16;
constexpr size_t NONCE_SIZE = 12;
constexpr size_t TAG_SIZE = 16;
constexpr size_t KEY_SIZE = 32;
constexpr uint32_t KDF_ITERATIONS = 200000;

/// Header = magic || version || kdf_iters(LE) || salt || nonce. The whole
/// header is fed to GCM as AAD so it is authenticated alongside the ciphertext.
constexpr size_t HEADER_SIZE = MAGIC_SIZE + 1 + 4 + SALT_SIZE + NONCE_SIZE;

/// Upper bound for the serialized plaintext / ciphertext. The plugins partition
/// is 2 MB; a semantic backup of all modules stays far below this.
constexpr size_t MAX_PAYLOAD = 256 * 1024;

/// Upper bound for the binary container (header + ciphertext + tag).
constexpr size_t MAX_CONTAINER = MAX_PAYLOAD + HEADER_SIZE + TAG_SIZE;

/// Upper bound for the base64-encoded container including a null terminator.
/// base64 expands 3 bytes -> 4 bytes, so ceiling(N/3)*4 + 1.
constexpr size_t MAX_BASE64 = ((MAX_CONTAINER + 2) / 3) * 4 + 1;

const char* backupPath() {
    static char path[64] = {0};
    if (path[0] == '\0') {
        std::snprintf(path, sizeof(path), "%s/backup.cdcbak",
                      cdc::plugin_manager::PluginStorage::basePath());
    }
    return path;
}

/// Writes a little-endian uint32 into a 4-byte slot.
void putU32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

/// Reads a little-endian uint32 from a 4-byte slot.
uint32_t getU32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/**
 * \brief Base64-encodes \p src into \p dst (null-terminated text).
 * \param dst  Output buffer of at least \c MAX_BASE64 bytes.
 * \param olen Receives the number of base64 bytes written (excluding null).
 * \return true on success.
 */
bool b64Encode(const uint8_t* src, size_t src_len,
               uint8_t* dst, size_t dst_cap, size_t* olen) {
    int ret = mbedtls_base64_encode(dst, dst_cap, olen, src, src_len);
    if (ret != 0) {
        LOG_E(TAG, "base64 encode failed: %d", ret);
        return false;
    }
    if (*olen < dst_cap) dst[*olen] = '\0';
    return true;
}

/**
 * \brief Base64-decodes \p src into \p dst.
 * \param olen Receives the decoded byte count.
 * \return true on success.
 */
bool b64Decode(const uint8_t* src, size_t src_len,
               uint8_t* dst, size_t dst_cap, size_t* olen) {
    int ret = mbedtls_base64_decode(dst, dst_cap, olen, src, src_len);
    if (ret != 0) {
        LOG_E(TAG, "base64 decode failed: %d", ret);
        return false;
    }
    return true;
}

/**
 * \brief Derives the 32-byte AES key from the passphrase via PBKDF2-HMAC-SHA256.
 * \return true on success.
 */
bool deriveKey(const char* passphrase, const uint8_t* salt, uint32_t iterations,
               uint8_t key_out[KEY_SIZE]) {
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        reinterpret_cast<const uint8_t*>(passphrase), std::strlen(passphrase),
        salt, SALT_SIZE,
        iterations, KEY_SIZE, key_out);
    return ret == 0;
}

/**
 * \brief Encrypts \p plaintext into a fully framed container in \p out.
 * \param out Caller buffer, must hold HEADER_SIZE + plaintext_len + TAG_SIZE.
 * \param out_len Receives the total container length.
 * \return true on success.
 */
bool sealContainer(const char* passphrase,
                   const uint8_t* plaintext, size_t plaintext_len,
                   uint8_t* out, size_t out_cap, size_t* out_len) {
    const size_t total = HEADER_SIZE + plaintext_len + TAG_SIZE;
    if (out_cap < total) return false;

    uint8_t* p_magic = out;
    uint8_t* p_version = p_magic + MAGIC_SIZE;
    uint8_t* p_iters = p_version + 1;
    uint8_t* p_salt = p_iters + 4;
    uint8_t* p_nonce = p_salt + SALT_SIZE;
    uint8_t* p_ct = out + HEADER_SIZE;
    uint8_t* p_tag = p_ct + plaintext_len;

    std::memcpy(p_magic, MAGIC, MAGIC_SIZE);
    *p_version = CONTAINER_VERSION;
    putU32le(p_iters, KDF_ITERATIONS);
    esp_fill_random(p_salt, SALT_SIZE);
    esp_fill_random(p_nonce, NONCE_SIZE);

    uint8_t key[KEY_SIZE];
    if (!deriveKey(passphrase, p_salt, KDF_ITERATIONS, key)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        return false;
    }

    bool ok = cdc::core::aesGcm256Seal(
        key, p_nonce, NONCE_SIZE,
        out, HEADER_SIZE,
        plaintext, plaintext_len,
        p_ct, p_tag);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (!ok) {
        LOG_E(TAG, "GCM encrypt failed");
        return false;
    }
    *out_len = total;
    return true;
}

/**
 * \brief Validates the header and authenticates+decrypts the container.
 * \param plaintext_out Caller buffer for the recovered JSON (must be >= ct len).
 * \param plaintext_len Receives the recovered plaintext length.
 * \return true on success (false on bad magic/version or GCM tag mismatch).
 */
bool openContainer(const char* passphrase,
                   const uint8_t* container, size_t container_len,
                   uint8_t* plaintext_out, size_t plaintext_cap,
                   size_t* plaintext_len) {
    if (container_len < HEADER_SIZE + TAG_SIZE) return false;
    if (std::memcmp(container, MAGIC, MAGIC_SIZE) != 0) {
        LOG_E(TAG, "Bad magic");
        return false;
    }
    if (container[MAGIC_SIZE] != CONTAINER_VERSION) {
        LOG_E(TAG, "Unsupported container version %u", container[MAGIC_SIZE]);
        return false;
    }

    const uint8_t* p_iters = container + MAGIC_SIZE + 1;
    const uint8_t* p_salt = p_iters + 4;
    const uint8_t* p_nonce = p_salt + SALT_SIZE;
    const uint8_t* p_ct = container + HEADER_SIZE;
    const size_t ct_len = container_len - HEADER_SIZE - TAG_SIZE;
    const uint8_t* p_tag = p_ct + ct_len;
    if (ct_len > plaintext_cap) return false;

    uint32_t iterations = getU32le(p_iters);

    uint8_t key[KEY_SIZE];
    if (!deriveKey(passphrase, p_salt, iterations, key)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        return false;
    }

    bool ok = cdc::core::aesGcm256Open(
        key, p_nonce, NONCE_SIZE,
        container, HEADER_SIZE,
        p_ct, ct_len,
        p_tag, plaintext_out);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (!ok) {
        mbedtls_platform_zeroize(plaintext_out, ct_len);
        LOG_W(TAG, "GCM decrypt/auth failed (wrong passphrase?)");
        return false;
    }
    *plaintext_len = ct_len;
    return true;
}

/// Reads a host API level "<major>.<minor>" into a packed comparable value.
uint32_t packApiLevel(const char* str) {
    unsigned major = 0, minor = 0;
    if (!str || std::sscanf(str, "%u.%u", &major, &minor) < 1) return 0;
    return (major << 16) | (minor & 0xFFFF);
}

} // namespace

BackupManager& BackupManager::instance() {
    static BackupManager mgr;
    return mgr;
}

bool BackupManager::exportTo(const char* passphrase) {
    if (!passphrase || passphrase[0] == '\0') {
        LOG_E(TAG, "Empty passphrase");
        return false;
    }

    bool ok = false;
    auto plain = cdc::core::psramAlloc<uint8_t>(MAX_PAYLOAD);
    auto container = cdc::core::psramAlloc<uint8_t>(MAX_CONTAINER);
    auto b64buf = cdc::core::psramAlloc<uint8_t>(MAX_BASE64);
    if (!plain || !container || !b64buf) {
        LOG_E(TAG, "PSRAM alloc failed");
        return false;
    }

    {
        cdc::ui::PsramCjsonScope psram;
        char* json_text = nullptr;
        cJSON* root = cJSON_CreateObject();
        if (!root) return false;

        cJSON_AddStringToObject(root, "host_api_level", HOST_API_LEVEL_STR);
        cJSON_AddStringToObject(root, "fw_version", APP_VERSION);
        cJSON* modules = cJSON_AddObjectToObject(root, "modules");

        auto& reg = cdc::core::ModuleRegistry::instance();
        uint8_t count = reg.getModuleCount();
        uint8_t exported = 0;
        for (uint8_t i = 0; modules && i < count; i++) {
            cdc::core::IModule* m = reg.getModuleAt(i);
            if (!m || !m->getName()) continue;

            // Each module fills a section node; absent modules emit nothing.
            cJSON* section = cJSON_CreateObject();
            if (!section) continue;
            if (m->exportBackup(section)) {
                cJSON_AddItemToObject(modules, m->getName(), section);
                exported++;
            } else {
                cJSON_Delete(section);
            }
        }
        LOG_I(TAG, "Exporting %u module section(s)", exported);

        // System/NVS settings have no owning module; emit them as a top-level
        // "system" section alongside "modules".
        cJSON* system = cJSON_CreateObject();
        if (system && SystemSettingsBackup::exportSystemSettings(system)) {
            cJSON_AddItemToObject(root, "system", system);
        } else if (system) {
            cJSON_Delete(system);
        }

        json_text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (!json_text) {
            LOG_E(TAG, "JSON serialization failed");
            return false;
        }

        size_t plain_len = std::strlen(json_text);
        if (plain_len <= MAX_PAYLOAD) {
            std::memcpy(plain.get(), json_text, plain_len);
            size_t container_len = 0;
            size_t b64_len = 0;
            if (sealContainer(passphrase, plain.get(), plain_len,
                              container.get(), MAX_CONTAINER,
                              &container_len) &&
                b64Encode(container.get(), container_len,
                          b64buf.get(), MAX_BASE64, &b64_len)) {
                auto f = cdc::core::openFile(backupPath(), "wb");
                if (f && std::fwrite(b64buf.get(), 1, b64_len, f.get()) == b64_len) {
                    ok = true;
                    LOG_I(TAG, "Backup written (%zu bytes base64)", b64_len);
                } else {
                    LOG_E(TAG, "Write to %s failed", backupPath());
                }
            }
        } else {
            LOG_E(TAG, "Payload too large: %zu", plain_len);
        }

        // Wipe every transient copy of the plaintext before releasing the
        // buffers. Free json_text inside the scope so it pairs with the PSRAM
        // hook that allocated it.
        mbedtls_platform_zeroize(json_text, plain_len);
        cJSON_free(json_text);
    }

    mbedtls_platform_zeroize(plain.get(), MAX_PAYLOAD);
    return ok;
}

BackupSummary BackupManager::importFrom(const char* passphrase) {
    BackupSummary summary;
    if (!passphrase || passphrase[0] == '\0' || !backupExists()) {
        return summary;
    }

    auto b64buf = cdc::core::psramAlloc<uint8_t>(MAX_BASE64);
    auto container = cdc::core::psramAlloc<uint8_t>(MAX_CONTAINER);
    auto plain = cdc::core::psramAlloc<uint8_t>(MAX_PAYLOAD + 1);
    if (!b64buf || !container || !plain) {
        LOG_E(TAG, "PSRAM alloc failed");
        return summary;
    }

    size_t b64_len = 0;
    {
        auto f = cdc::core::openFile(backupPath(), "rb");
        if (!f) return summary;
        b64_len = std::fread(b64buf.get(), 1, MAX_BASE64 - 1, f.get());
    }

    size_t container_len = 0;
    if (!b64Decode(b64buf.get(), b64_len,
                   container.get(), MAX_CONTAINER, &container_len)) {
        return summary;
    }

    size_t plain_len = 0;
    if (!openContainer(passphrase, container.get(), container_len,
                       plain.get(), MAX_PAYLOAD, &plain_len)) {
        return summary;
    }
    plain.get()[plain_len] = '\0';

    {
        cdc::ui::PsramCjsonScope psram;
        cJSON* root = cJSON_Parse(reinterpret_cast<const char*>(plain.get()));
        if (root) {
            const cJSON* level = cJSON_GetObjectItemCaseSensitive(root, "host_api_level");
            uint32_t file_level = packApiLevel(cJSON_IsString(level) ? level->valuestring : nullptr);
            if (file_level > HOST_API_LEVEL_PACKED) {
                LOG_E(TAG, "Backup host_api_level too new (%s > %s)",
                      cJSON_IsString(level) ? level->valuestring : "?", HOST_API_LEVEL_STR);
            } else {
                const cJSON* modules = cJSON_GetObjectItemCaseSensitive(root, "modules");
                auto& reg = cdc::core::ModuleRegistry::instance();
                for (const cJSON* sec = modules ? modules->child : nullptr;
                     sec != nullptr; sec = sec->next) {
                    if (!sec->string) continue;
                    cdc::core::IModule* m = reg.getModule(sec->string);
                    if (!m) {
                        LOG_W(TAG, "No module '%s' on device, skipping section", sec->string);
                        summary.skipped++;
                        continue;
                    }
                    cdc::core::IModule::BackupResult r = m->importBackup(sec);
                    summary.imported += r.imported;
                    summary.failed += r.failed;
                    summary.modules++;
                }

                // Route the top-level "system" section (no owning module).
                const cJSON* system = cJSON_GetObjectItemCaseSensitive(root, "system");
                if (cJSON_IsObject(system)) {
                    cdc::core::IModule::BackupResult r =
                        SystemSettingsBackup::importSystemSettings(system);
                    summary.imported += r.imported;
                    summary.failed += r.failed;
                    summary.system = true;
                }

                summary.ok = true;
            }
        } else {
            LOG_E(TAG, "JSON parse failed");
        }
        cJSON_Delete(root);
    }

    mbedtls_platform_zeroize(plain.get(), MAX_PAYLOAD + 1);
    LOG_I(TAG, "Import done: %u imported, %u failed, %u modules, %u skipped",
          summary.imported, summary.failed, summary.modules, summary.skipped);
    return summary;
}

bool BackupManager::backupExists() const {
    struct stat st;
    return stat(backupPath(), &st) == 0;
}

bool BackupManager::deleteBackup() {
    if (!backupExists()) return true;
    return std::remove(backupPath()) == 0;
}

} // namespace cdc::os_ui
