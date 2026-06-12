#pragma once

#include <cstdint>

namespace cdc::os_ui {

/**
 * \brief Aggregated outcome of a restore across all modules.
 */
struct BackupSummary {
    bool ok = false;          ///< Container decrypted and parsed successfully.
    uint16_t imported = 0;    ///< Total records restored (modules + system section).
    uint16_t failed = 0;      ///< Total records skipped due to errors.
    uint8_t modules = 0;      ///< Number of module sections that were applied.
    uint8_t skipped = 0;      ///< Module sections with no matching module on-device.
    bool system = false;      ///< System/NVS settings section was present and applied.
};

/**
 * \brief Central, data-agnostic backup loader.
 *
 * Owns the encrypted container I/O for the single on-device backup file. It
 * iterates the module registry, lets each module emit/consume only its own JSON
 * section (via IModule::exportBackup / importBackup), and never inspects module
 * payloads itself.
 *
 * Container format (binary):
 *   magic[6] "CDCBAK" || version(1) || kdf_iters(uint32 LE) || salt(16) ||
 *   nonce(12) || ciphertext(N) || gcm_tag(16)
 * The plaintext is a JSON document; only the ciphertext (incl. tag) is ever
 * present in the container. Key = PBKDF2-HMAC-SHA256(passphrase, salt,
 * kdf_iters) -> 32 bytes; cipher = AES-256-GCM with the header bytes as AAD.
 * The binary container is stored base64-encoded on vFAT for text-safe serial
 * transfer. The same format is reproduced by tools/backup.py.
 */
class BackupManager {
public:
    /**
     * \brief Returns the process-wide singleton.
     */
    static BackupManager& instance();

    /**
     * \brief Exports all module sections into one encrypted backup file.
     *
     * Overwrites any existing backup. The serialized JSON lives only transiently
     * in PSRAM and is zeroized after encryption.
     *
     * \param passphrase Export passphrase (must be non-empty).
     * \return true if the encrypted file was written.
     */
    bool exportTo(const char* passphrase);

    /**
     * \brief Restores from the on-device backup file (best-effort).
     *
     * Reads and decrypts the container, parses the JSON, gates compatibility on
     * the host API level, then routes each section to its module. Unknown
     * modules are skipped and counted; never aborts on a per-module failure.
     *
     * \param passphrase Passphrase used at export time.
     * \return Aggregated summary; \c ok is false on read/decrypt/parse failure.
     */
    BackupSummary importFrom(const char* passphrase);

    /**
     * \brief Reports whether a backup file is present on the device.
     */
    bool backupExists() const;

    /**
     * \brief Deletes the on-device backup file.
     * \return true if the file was removed (or already absent).
     */
    bool deleteBackup();

private:
    BackupManager() = default;
    BackupManager(const BackupManager&) = delete;
    BackupManager& operator=(const BackupManager&) = delete;
};

} // namespace cdc::os_ui
