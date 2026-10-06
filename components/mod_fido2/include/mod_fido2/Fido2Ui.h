#pragma once

#include "mod_fido2/fido2.h"
#include "cdc_ui/IView.h"

namespace cdc::mod_fido2 {

void fido2_ui_init();
cdc::ui::IView* fido2_ui_get_list_view();
const char* fido2_ui_get_label();
fido2_user_presence_result_t fido2_ui_user_presence_callback(
    const char* rp_id,
    fido2_action_t action,
    const char* user_name
);

/**
 * \brief Forcibly denies any in-flight user-presence prompt.
 * \return true if a prompt was active and got denied, false otherwise.
 */
bool fido2_ui_abort_prompt();

/**
 * \brief Records whether the badge is on the lock screen.
 *        A locked badge asks for the badge PIN before approving a prompt.
 * \param locked `true` when the badge is locked.
 */
void fido2_ui_set_locked(bool locked);

} // namespace cdc::mod_fido2
