#include "plugin_manager/PluginStorage.h"
#include "cdc_core/Raii.h"
#include "cdc_core/feature_flags.h"
#include "cdc_log.h"

#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "ff.h"
#include "usb_badge/usb_msc_bounds.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_STO";
static const char* PARTITION_LABEL = "vfat";
static const char* MOUNT_POINT = "/vfat";
// System files (plugins, i18n) live in a "system" subfolder so the partition
// root is a safe, browsable user area.
static const char* SYSTEM_DIR = "/vfat/system";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_mounted = false;
static bool s_host_active = false;
static bool s_remount_pending = false;

// FAT volume label shown to a USB host (uppercase per FAT label convention).
static const char* VOLUME_LABEL = "CDCBADGE";

// esp_vfs_fat assigns the FatFs drive number internally; locate ours by the
// presence of the system folder. Returns the drive index, or -1 if not found.
static int vfat_drive()
{
    FILINFO fno;
    char path[32];
    for (int d = 0; d < FF_VOLUMES; ++d) {
        snprintf(path, sizeof(path), "%d:/system", d);
        if (f_stat(path, &fno) == FR_OK) return d;
    }
    return -1;
}

// Set the FAT volume label so the host shows the drive as CDCBADGE instead of
// the fatfsgen default. Idempotent; skipped while a host holds the volume.
static void ensure_volume_label()
{
    if (!s_mounted || s_host_active) return;
    const int drive = vfat_drive();
    if (drive < 0) return;
    char drv[16];
    snprintf(drv, sizeof(drv), "%d:", drive);
    char cur[16] = {0};
    if (f_getlabel(drv, cur, nullptr) == FR_OK && std::strcmp(cur, VOLUME_LABEL) == 0) {
        return;  // already correct
    }
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "%d:%s", drive, VOLUME_LABEL);
    if (f_setlabel(lbl) == FR_OK) {
        LOG_I(TAG, "set vfat volume label to %s", VOLUME_LABEL);
    } else {
        LOG_W(TAG, "f_setlabel failed");
    }
}

bool PluginStorage::mount()
{
    if (s_mounted) return true;

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        MOUNT_POINT, PARTITION_LABEL, &cfg, &s_wl_handle);
    if (err != ESP_OK) {
        LOG_E(TAG, "mount failed: 0x%x", err);
        return false;
    }

    LOG_I(TAG, "mounted %s on %s", PARTITION_LABEL, MOUNT_POINT);
    s_mounted = true;
    mkdir(SYSTEM_DIR, 0777);  // ensure the hidden system folder exists
    ensure_volume_label();
    protectSystemDir();
    return true;
}

void PluginStorage::unmount()
{
    if (!s_mounted) return;
    esp_vfs_fat_spiflash_unmount_rw_wl(MOUNT_POINT, s_wl_handle);
    s_wl_handle = WL_INVALID_HANDLE;
    s_mounted = false;
}

const char* PluginStorage::basePath()
{
    return MOUNT_POINT;
}

static bool ends_with(const char* s, size_t s_len, const char* suffix, size_t suf_len)
{
    return s_len > suf_len && std::strcmp(s + s_len - suf_len, suffix) == 0;
}

std::vector<std::string> PluginStorage::listPluginIds()
{
    std::vector<std::string> ids;
    if (!s_mounted) return ids;

    DIR* dir = opendir(SYSTEM_DIR);
    if (!dir) {
        return ids;  // not yet created (fresh format) -> no plugins
    }

    while (struct dirent* ent = readdir(dir)) {
        const char* name = ent->d_name;
        size_t len = std::strlen(name);
        std::string id;
        if (ends_with(name, len, ".aot", 4)) {
            id.assign(name, len - 4);
        } else if (ends_with(name, len, ".wasm", 5)) {
            id.assign(name, len - 5);
        } else {
            continue;
        }
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;

        std::string meta = metaPath(id);
        struct stat st;
        if (stat(meta.c_str(), &st) == 0 && (st.st_mode & S_IFREG)) {
            ids.push_back(std::move(id));
        }
    }

    closedir(dir);
    return ids;
}

std::string PluginStorage::binaryPath(const std::string& id)
{
#if FEATURE_PLUGIN_AOT
    std::string aot = aotPath(id);
    struct stat st;
    if (stat(aot.c_str(), &st) == 0 && (st.st_mode & S_IFREG)) {
        return aot;
    }
#endif
    return wasmPath(id);
}

std::string PluginStorage::wasmPath(const std::string& id)
{
    return std::string(SYSTEM_DIR) + "/" + id + ".wasm";
}

std::string PluginStorage::aotPath(const std::string& id)
{
    return std::string(SYSTEM_DIR) + "/" + id + ".aot";
}

std::string PluginStorage::metaPath(const std::string& id)
{
    return std::string(SYSTEM_DIR) + "/" + id + ".meta";
}

std::string PluginStorage::langPath(const std::string& id)
{
    return std::string(SYSTEM_DIR) + "/" + id + ".lang";
}

std::string PluginStorage::disabledPath(const std::string& id)
{
    return std::string(SYSTEM_DIR) + "/" + id + ".disabled";
}

bool PluginStorage::isDisabled(const std::string& id)
{
    struct stat st;
    const std::string path = disabledPath(id);
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}

bool PluginStorage::setDisabled(const std::string& id, bool disabled)
{
    const std::string path = disabledPath(id);
    if (!disabled) {
        errno = 0;
        return std::remove(path.c_str()) == 0 || errno == ENOENT;
    }

    auto fp = ::cdc::core::openFile(path.c_str(), "wb");
    if (!fp) return false;
    static constexpr char kMarker[] = "disabled\n";
    return std::fwrite(kMarker, 1, sizeof(kMarker) - 1, fp.get()) == sizeof(kMarker) - 1;
}

bool PluginStorage::stats(uint64_t& free_bytes, uint64_t& total_bytes)
{
    if (!s_mounted) return false;
    return esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK;
}

uint16_t PluginStorage::blockSize()
{
    if (!s_mounted) return 0;
    return static_cast<uint16_t>(wl_sector_size(s_wl_handle));
}

uint64_t PluginStorage::blockTotalBytes()
{
    if (!s_mounted) return 0;
    return wl_size(s_wl_handle);
}

bool PluginStorage::blockRead(uint32_t lba, uint32_t offset, void* buf, uint32_t len)
{
    if (!s_mounted || !buf) return false;
    const uint16_t bs = static_cast<uint16_t>(wl_sector_size(s_wl_handle));
    if (!usb_msc_range_ok(wl_size(s_wl_handle), bs, lba, offset, len, false)) return false;
    const uint64_t addr = static_cast<uint64_t>(lba) * bs + offset;
    return wl_read(s_wl_handle, static_cast<size_t>(addr), buf, len) == ESP_OK;
}

bool PluginStorage::blockWrite(uint32_t lba, uint32_t offset, const void* buf, uint32_t len)
{
    if (!s_mounted || !buf) return false;
    const uint16_t bs = static_cast<uint16_t>(wl_sector_size(s_wl_handle));
    // Wear-levelling writes are erase-then-write at sector granularity, so the
    // MSC layer must hand us whole, sector-aligned blocks.
    if (!usb_msc_range_ok(wl_size(s_wl_handle), bs, lba, offset, len, true)) return false;
    const uint64_t addr = static_cast<uint64_t>(lba) * bs;
    if (wl_erase_range(s_wl_handle, static_cast<size_t>(addr), len) != ESP_OK) return false;
    return wl_write(s_wl_handle, static_cast<size_t>(addr), buf, len) == ESP_OK;
}

void PluginStorage::setHostActive(bool active)
{
    if (active == s_host_active) return;
    const bool wasActive = s_host_active;
    s_host_active = active;
    LOG_I(TAG, "MSC host %s", active ? "attached" : "detached");
    // Defer the remount out of the USB callback context; remountIfPending()
    // performs it on a safe task so host-written files become visible.
    if (usb_msc_should_remount(wasActive, active)) {
        s_remount_pending = true;
    }
}

bool PluginStorage::hostActive()
{
    return s_host_active;
}

void PluginStorage::remountIfPending()
{
    if (!s_remount_pending || s_host_active || !s_mounted) return;
    s_remount_pending = false;
    unmount();
    mount();
    LOG_I(TAG, "remounted %s after MSC host detach", MOUNT_POINT);
}

void PluginStorage::protectSystemDir()
{
    if (!s_mounted || s_host_active) return;

    static constexpr BYTE kAttr = AM_RDO | AM_HID | AM_SYS;
    static constexpr BYTE kMask = AM_RDO | AM_HID | AM_SYS;

    const int drive = vfat_drive();
    if (drive < 0) {
        LOG_W(TAG, "protectSystemDir: system folder not found for chmod");
        return;
    }
    // Hide the directory itself but leave it writable (badge writes into it when
    // no host is attached); read-only is applied to the files below.
    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), "%d:/system", drive);
    f_chmod(dirPath, AM_HID | AM_SYS, AM_RDO | AM_HID | AM_SYS);

    // Mark each system file read-only + hidden + system so a host file manager
    // hides them and refuses to modify or delete them (advisory only).
    DIR* dir = opendir(SYSTEM_DIR);
    if (!dir) return;
    char filePath[280];  // "N:/system/" + up to a 255-char FAT long name
    while (struct dirent* ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        snprintf(filePath, sizeof(filePath), "%d:/system/%s", drive, ent->d_name);
        f_chmod(filePath, kAttr, kMask);
    }
    closedir(dir);
    LOG_I(TAG, "protectSystemDir: marked system folder on drive %d", drive);
}

}  // namespace cdc::plugin_manager
