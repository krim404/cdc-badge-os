#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ESP32-S3 USB-OTG silicon endpoint limits (TinyUSB dwc2_esp32.h:
 * ep_count = 7, ep_in_count = 5): EP0 plus six endpoint numbers per
 * direction, but at most five concurrently active IN endpoints including
 * EP0. The non-control budget is therefore 4 IN and 6 OUT endpoints.
 */
#define USB_EP_BUDGET_MAX_IN 4
#define USB_EP_BUDGET_MAX_OUT 6

/** Fixed CDC-ACM cost: interrupt notification IN + bulk IN + bulk OUT. */
#define USB_EP_BUDGET_CDC_IN 2
#define USB_EP_BUDGET_CDC_OUT 1

/** Fixed MSC cost: one bulk endpoint per direction. */
#define USB_EP_BUDGET_MSC_IN 1
#define USB_EP_BUDGET_MSC_OUT 1

/**
 * \brief Non-control endpoint usage split by direction.
 */
typedef struct {
    uint8_t in_eps;
    uint8_t out_eps;
} usb_ep_usage_t;

/**
 * \brief Tests whether an additional endpoint demand fits the silicon budget.
 * \param used Endpoints already consumed by the active configuration.
 * \param add Endpoints the new interface would consume.
 * \return `true` when both directions stay within the hardware limits.
 */
static inline bool usb_ep_budget_fits(usb_ep_usage_t used, usb_ep_usage_t add) {
    return (uint8_t)(used.in_eps + add.in_eps) <= USB_EP_BUDGET_MAX_IN &&
           (uint8_t)(used.out_eps + add.out_eps) <= USB_EP_BUDGET_MAX_OUT;
}

#ifdef __cplusplus
}
#endif
