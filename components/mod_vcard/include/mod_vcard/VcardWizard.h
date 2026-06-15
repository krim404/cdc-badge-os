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
     * \brief Resolves a localized step title.
     * \param baseId The base i18n string ID of the owning module.
     * \param offset Per-module string offset within the registered range.
     * \return Translated string pointer.
     */
    using StringResolver = const char* (*)(uint16_t offset);

    /**
     * \brief Configures the wizard with i18n callbacks. Must be called before
     *        start() or edit() so step titles can be looked up.
     * \param resolver Function returning a translated title for a string offset.
     * \param titleOffsets Pointer to an array of 16 offsets for the wizard steps.
     * \param savedOffset Offset of the "saved" toast message.
     * \param failedOffset Offset of the generic "failed" toast message.
     */
    static void configure(StringResolver resolver, const uint16_t* titleOffsets,
                          uint16_t savedOffset, uint16_t failedOffset);

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
