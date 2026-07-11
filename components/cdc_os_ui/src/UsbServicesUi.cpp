/**
 * \file
 * \brief USB service toggle list under Tools: per-service on/off with endpoint
 *        budget feedback and re-enumeration handling.
 */

#include "AppUiInternal.h"
#include "cdc_os_ui/AppUi.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/UsbServiceManager.h"

#include <cstdio>
#include <cstring>

namespace cdc::ui {

// One row per service plus a trailing, non-actionable endpoint budget row.
static constexpr uint8_t USBSVC_VIEW_MAX = core::UsbServiceManager::MAX_SERVICES + 1;

static ListView* s_usbSvcView = nullptr;
static ListItem s_usbSvcItems[USBSVC_VIEW_MAX];
static char s_usbSvcLabels[USBSVC_VIEW_MAX][48];

static void rebuildUsbServicesView();

/**
 * \brief Short state marker for one service row.
 */
static const char* stateMarker(core::UsbServiceState state) {
    switch (state) {
        case core::UsbServiceState::On:        return "[ON]";
        case core::UsbServiceState::Off:       return "[OFF]";
        case core::UsbServiceState::Suspended: return "[SUSP]";
        case core::UsbServiceState::Unavailable:
        default:                               return "[--]";
    }
}

/**
 * \brief Runs the toggle and maps the result to user feedback.
 * \param id Service id.
 * \param enabled Desired state.
 */
static void toggleUsbService(const char* id, bool enabled) {
    using Result = core::UsbServiceManager::ToggleResult;

    const bool needsReplugBefore = core::UsbManager::instance().needsReplug();

    switch (core::UsbServiceManager::instance().setEnabled(id, enabled)) {
        case Result::Ok:
            showToastSuccess(ui::tr("core.ok"), TOAST_DURATION_SHORT_MS);
            break;
        case Result::BudgetFull:
            showToastError(ui::tr("core.usb_no_free_slot"), TOAST_DURATION_MEDIUM_MS);
            break;
        case Result::SlotBusy:
            showToastError(ui::tr("core.usbsvc_slot_busy"), TOAST_DURATION_MEDIUM_MS);
            break;
        case Result::Busy:
            showToastInfo(ui::tr("core.usbsvc_busy"), TOAST_DURATION_MEDIUM_MS);
            break;
        case Result::Failed:
        case Result::NotFound:
        default:
            showToastError(ui::tr("core.failed"), TOAST_DURATION_MEDIUM_MS);
            break;
    }

    if (core::UsbManager::instance().newlyRequiresReplug(needsReplugBefore)) {
        showToastAlertSticky(ui::tr("core.usb_replug_required"));
    }

    ui_rebuild_menus();
    rebuildUsbServicesView();
}

/**
 * \brief Confirm callback for disabling the CDC serial console.
 * \param userData Unused.
 */
static void onCdcDisableConfirm(void* userData) {
    (void)userData;
    toggleUsbService("cdc", false);
}

/**
 * \brief Handles service row selection: toggles or asks for confirmation.
 * \param index Selected row index.
 * \param userData Unused.
 */
static void onUsbServiceSelect(uint16_t index, void* userData) {
    (void)userData;

    auto& mgr = core::UsbServiceManager::instance();
    if (index >= mgr.count()) return;  // endpoint budget row

    const core::UsbServiceDesc* desc = mgr.at(static_cast<uint8_t>(index));
    if (!desc) return;

    const core::UsbServiceState state = mgr.state(static_cast<uint8_t>(index));
    if (state == core::UsbServiceState::Suspended) {
        showToastInfo(ui::tr("core.usbsvc_busy"), TOAST_DURATION_MEDIUM_MS);
        return;
    }

    const bool enable = (state != core::UsbServiceState::On);

    // Disabling the serial console kills the USB console immediately; make
    // sure the user knows this menu is the way back.
    if (!enable && strcmp(desc->id, "cdc") == 0) {
        showConfirm(ui::tr("core.usbsvc_cdc_confirm"), onCdcDisableConfirm, nullptr,
                    ConfirmView::Icon::WARNING);
        return;
    }

    toggleUsbService(desc->id, enable);
}

/**
 * \brief Rebuilds service rows and the endpoint budget footer.
 */
static void rebuildUsbServicesView() {
    auto& mgr = core::UsbServiceManager::instance();
    const uint8_t count = mgr.count();

    for (uint8_t i = 0; i < count; i++) {
        const core::UsbServiceDesc* desc = mgr.at(i);
        if (!desc) continue;
        snprintf(s_usbSvcLabels[i], sizeof(s_usbSvcLabels[i]), "%s %s",
                 ui::tr(desc->labelKey), stateMarker(mgr.state(i)));
        s_usbSvcItems[i] = {s_usbSvcLabels[i], 0, false, nullptr};
    }

    const usb_ep_usage_t usage = mgr.usage();
    snprintf(s_usbSvcLabels[count], sizeof(s_usbSvcLabels[count]), "EP IN %u/%u OUT %u/%u",
             usage.in_eps, USB_EP_BUDGET_MAX_IN, usage.out_eps, USB_EP_BUDGET_MAX_OUT);
    s_usbSvcItems[count] = {s_usbSvcLabels[count], 0, false, nullptr};

    if (s_usbSvcView) {
        s_usbSvcView->init(ui::tr("core.usb_services"), s_usbSvcItems,
                           static_cast<uint16_t>(count + 1));
    }
}

/**
 * \brief Shows the USB services toggle list.
 */
void showUsbServicesMenu() {
    if (!s_usbSvcView) {
        s_usbSvcView = new ListView();
    }

    rebuildUsbServicesView();
    s_usbSvcView->setOnSelect(onUsbServiceSelect);
    ViewStack::instance().push(s_usbSvcView);
}

} // namespace cdc::ui
