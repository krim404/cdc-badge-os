// USB Mass Storage (MSC) block-device glue.
//
// usb_badge owns the TinyUSB MSC class callbacks but stays free of any storage
// dependency: a module (mod_msc) wires the LUN to the vfat wear-levelling
// volume by installing this backend. With no backend installed the LUN reports
// "not ready" and the host sees no usable medium.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Block-device backend for the single USB Mass Storage LUN.
 */
typedef struct {
    bool     (*read)(uint32_t lba, uint32_t offset, void* buf, uint32_t len);
    bool     (*write)(uint32_t lba, uint32_t offset, const void* buf, uint32_t len);
    uint64_t (*total_bytes)(void);
    uint16_t (*block_size)(void);
    void     (*set_host_active)(bool active);
} usb_msc_backend_t;

/**
 * \brief Installs or removes the MSC block backend.
 * \param backend Backend to use, or NULL to detach (LUN reports not ready).
 */
void usb_msc_set_backend(const usb_msc_backend_t* backend);

#ifdef __cplusplus
}
#endif
