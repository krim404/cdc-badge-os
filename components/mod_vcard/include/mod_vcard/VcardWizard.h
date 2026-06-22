#pragma once

#include <cstdint>

#include "cdc_ui/IView.h"

namespace cdc::mod_vcard {

/**
 * \brief Wizard for creating or editing the device owner's vCard via the GUI.
 *
 * The wizard is a sequence of T9 input steps. On completion it generates a
 * vCard 4.0 string and persists it through `vcard_store_set_own()`.
 */
class VcardWizard {
public:
    /**
     * \brief Configures the wizard with i18n keys. Must be called before
     *        start() or edit() so step titles can be looked up.
     * \param titleKeys Pointer to an array of 16 i18n keys for the wizard steps.
     * \param savedKey i18n key of the "saved" toast message.
     * \param failedKey i18n key of the generic "failed" toast message.
     */
    static void configure(const char* const* titleKeys, const char* savedKey,
                          const char* failedKey);

    /**
     * \brief Callback fired after a successful save, before returning to the
     *        anchor view. Lets the caller refresh a list it owns.
     */
    using DoneCallback = void (*)();

    /**
     * \brief Starts the wizard with an empty struct.
     * \param returnAnchor View to pop back to once the wizard finishes.
     */
    static void start(ui::IView* returnAnchor);

    /**
     * \brief Starts the wizard prefilled with the currently stored own vCard.
     *        Falls back to start() when no vCard is present.
     * \param returnAnchor View to pop back to once the wizard finishes.
     */
    static void edit(ui::IView* returnAnchor);

    /**
     * \brief Starts the wizard to create a new stored contact (received list).
     * \param returnAnchor View to pop back to once the wizard finishes.
     * \param onDone Optional callback fired after a successful save.
     */
    static void startReceived(ui::IView* returnAnchor, DoneCallback onDone);

    /**
     * \brief Starts the wizard prefilled with a stored contact for editing.
     * \param returnAnchor View to pop back to once the wizard finishes.
     * \param slot Slot index of the stored contact to edit.
     * \param onDone Optional callback fired after a successful save.
     */
    static void editReceived(ui::IView* returnAnchor, uint16_t slot, DoneCallback onDone);
};

} // namespace cdc::mod_vcard
