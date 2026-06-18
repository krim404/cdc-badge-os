/**
 * \brief TinyUSB Mass Storage (MSC) class callbacks for the single vfat LUN.
 *
 * The block device is provided at runtime by a backend (see usb_msc.h) so this
 * file carries no storage dependency. All callbacks target LUN 0; reads/writes
 * are bounds-checked inside the backend.
 */

#include "usb_badge/usb_msc.h"
#include "cdc_log.h"

extern "C" {
#include "tusb.h"
#include "class/msc/msc.h"
#include "class/msc/msc_device.h"
}

#include <cstring>

static const char* TAG = "USB_MSC";

static const usb_msc_backend_t* s_backend = nullptr;

extern "C" void usb_msc_set_backend(const usb_msc_backend_t* backend) {
    s_backend = backend;
    LOG_I(TAG, "MSC backend %s", backend ? "attached" : "detached");
}

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "CDCBadge";
    const char pid[] = "vFAT Storage";
    const char rev[] = "1.0";
    std::memset(vendor_id, ' ', 8);
    std::memset(product_id, ' ', 16);
    std::memset(product_rev, ' ', 4);
    std::memcpy(vendor_id, vid, std::strlen(vid) > 8 ? 8 : std::strlen(vid));
    std::memcpy(product_id, pid, std::strlen(pid) > 16 ? 16 : std::strlen(pid));
    std::memcpy(product_rev, rev, std::strlen(rev) > 4 ? 4 : std::strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return s_backend != nullptr;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    if (!s_backend) {
        *block_count = 0;
        *block_size = 0;
        return;
    }
    const uint16_t bs = s_backend->block_size();
    *block_size = bs;
    *block_count = bs ? static_cast<uint32_t>(s_backend->total_bytes() / bs) : 0;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return s_backend != nullptr;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer,
                          uint32_t bufsize) {
    (void)lun;
    if (!s_backend) return -1;
    // A host reading the LUN means it has the volume; gate badge-side writes.
    if (s_backend->set_host_active) s_backend->set_host_active(true);
    if (!s_backend->read(lba, offset, buffer, bufsize)) return -1;
    return static_cast<int32_t>(bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer,
                           uint32_t bufsize) {
    (void)lun;
    if (!s_backend) return -1;
    if (s_backend->set_host_active) s_backend->set_host_active(true);
    if (!s_backend->write(lba, offset, buffer, bufsize)) return -1;
    return static_cast<int32_t>(bufsize);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
    (void)lun;
    (void)power_condition;
    // An explicit eject releases the volume back to the badge.
    if (load_eject && !start && s_backend && s_backend->set_host_active) {
        s_backend->set_host_active(false);
    }
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer,
                        uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;
    (void)scsi_cmd;
    // Reject any SCSI command not handled by the class driver.
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

void tud_umount_cb(void) {
    // USB disconnect (including our own re-enumeration): release the volume so
    // the badge regains write access and refreshes its view.
    if (s_backend && s_backend->set_host_active) s_backend->set_host_active(false);
}

} // extern "C"
