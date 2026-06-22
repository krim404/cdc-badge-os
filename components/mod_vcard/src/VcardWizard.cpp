#include "mod_vcard/VcardWizard.h"
#include "mod_vcard/vcard_store.h"

#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"
#include "esp_attr.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "VCARD_WIZ";

namespace cdc::mod_vcard {

/**
 * \brief Step indices into the wizard's title offset table.
 *        Order must match `s_steps[]` below.
 */
enum WizardStepId : uint8_t {
    STEP_GIVEN = 0,
    STEP_FAMILY,
    STEP_FORMATTED,
    STEP_ORG,
    STEP_TITLE,
    STEP_EMAIL,
    STEP_TEL_CELL,
    STEP_TEL_HOME,
    STEP_TEL_WORK,
    STEP_URL,
    STEP_TELEGRAM,
    STEP_SIGNAL,
    STEP_MATRIX,
    STEP_THREEMA,
    STEP_SOCIAL,
    STEP_NOTE,
    STEP_COUNT
};

/**
 * \brief Maps a step ID to the matching field inside `vcard_data_t`.
 */
struct FieldRef {
    char* (*get)(vcard_data_t* d);
    uint16_t maxLen;
};

static char* fGiven(vcard_data_t* d)    { return d->given_name; }
static char* fFamily(vcard_data_t* d)   { return d->family_name; }
static char* fFn(vcard_data_t* d)       { return d->formatted_name; }
static char* fOrg(vcard_data_t* d)      { return d->organization; }
static char* fTitle(vcard_data_t* d)    { return d->title; }
static char* fEmail(vcard_data_t* d)    { return d->email; }
static char* fCell(vcard_data_t* d)     { return d->tel_cell; }
static char* fHome(vcard_data_t* d)     { return d->tel_home; }
static char* fWork(vcard_data_t* d)     { return d->tel_work; }
static char* fUrl(vcard_data_t* d)      { return d->url; }
static char* fTele(vcard_data_t* d)     { return d->impp_telegram; }
static char* fSig(vcard_data_t* d)      { return d->impp_signal; }
static char* fMatrix(vcard_data_t* d)   { return d->impp_matrix; }
static char* fThreema(vcard_data_t* d)  { return d->impp_threema; }
static char* fSocial(vcard_data_t* d)   { return d->social_profile; }
static char* fNote(vcard_data_t* d)     { return d->note; }

/**
 * \brief Step descriptor table. Step order is fixed at compile time.
 *        The maximum input length for each step is clamped to the smaller of
 *        the struct field size and the T9 input view's text limit.
 */
static const FieldRef k_steps[STEP_COUNT] = {
    { fGiven,   sizeof(vcard_data_t::given_name)     - 1 },
    { fFamily,  sizeof(vcard_data_t::family_name)    - 1 },
    { fFn,      sizeof(vcard_data_t::formatted_name) - 1 },
    { fOrg,     sizeof(vcard_data_t::organization)   - 1 },
    { fTitle,   sizeof(vcard_data_t::title)          - 1 },
    { fEmail,   sizeof(vcard_data_t::email)          - 1 },
    { fCell,    sizeof(vcard_data_t::tel_cell)       - 1 },
    { fHome,    sizeof(vcard_data_t::tel_home)       - 1 },
    { fWork,    sizeof(vcard_data_t::tel_work)       - 1 },
    { fUrl,     sizeof(vcard_data_t::url)            - 1 },
    { fTele,    sizeof(vcard_data_t::impp_telegram)  - 1 },
    { fSig,     sizeof(vcard_data_t::impp_signal)    - 1 },
    { fMatrix,  sizeof(vcard_data_t::impp_matrix)    - 1 },
    { fThreema, sizeof(vcard_data_t::impp_threema)   - 1 },
    { fSocial,  sizeof(vcard_data_t::social_profile) - 1 },
    { fNote,    sizeof(vcard_data_t::note)           - 1 },
};

/**
 * \brief Save destination for the wizard's generated vCard.
 */
enum class WizardTarget : uint8_t {
    OWN = 0,        ///< Persist via vcard_store_set_own().
    RECEIVED_NEW,   ///< Add a new stored contact.
    RECEIVED_EDIT,  ///< Overwrite the stored contact at editSlot.
};

/**
 * \brief Holds the wizard's running state between callback firings.
 */
struct WizardState {
    vcard_data_t data;
    ui::IView* returnAnchor;
    bool active;
    uint8_t currentStep;
    WizardTarget target;
    uint16_t editSlot;
    VcardWizard::DoneCallback onDone;
};

EXT_RAM_BSS_ATTR static WizardState s_wizard = {};
static ui::T9InputView s_t9Input;

static const char* const* s_titleKeys = nullptr;
static const char* s_savedKey = nullptr;
static const char* s_failedKey = nullptr;

/**
 * \brief Returns the localized title for a wizard step via its i18n key.
 */
static const char* stepTitle(uint8_t step) {
    if (!s_titleKeys || step >= STEP_COUNT) return "?";
    return ui::tr(s_titleKeys[step]);
}

/**
 * \brief Forward declaration of the single per-step save handler.
 */
static void onStepSave(const char* text);

/**
 * \brief Pushes the T9 input view for the current wizard step.
 *        Prefills the input with the current field value (which may have been
 *        seeded from a pre-existing vCard in edit mode, or auto-suggested for FN).
 */
static void pushCurrentStep() {
    if (!s_wizard.active || s_wizard.currentStep >= STEP_COUNT) return;

    const FieldRef& ref = k_steps[s_wizard.currentStep];
    char* field = ref.get(&s_wizard.data);

    // Auto-suggest formatted name from given + family when not yet set.
    if (s_wizard.currentStep == STEP_FORMATTED && field[0] == '\0' &&
        (s_wizard.data.given_name[0] || s_wizard.data.family_name[0])) {
        if (s_wizard.data.given_name[0] && s_wizard.data.family_name[0]) {
            snprintf(field, sizeof(vcard_data_t::formatted_name), "%s %s",
                     s_wizard.data.given_name, s_wizard.data.family_name);
        } else if (s_wizard.data.given_name[0]) {
            snprintf(field, sizeof(vcard_data_t::formatted_name), "%s",
                     s_wizard.data.given_name);
        } else {
            snprintf(field, sizeof(vcard_data_t::formatted_name), "%s",
                     s_wizard.data.family_name);
        }
    }

    uint16_t maxLen = ref.maxLen;
    if (maxLen > ui::T9InputView::MAX_TEXT_LEN) maxLen = ui::T9InputView::MAX_TEXT_LEN;

    s_t9Input.init(stepTitle(s_wizard.currentStep), field, maxLen);
    s_t9Input.setOnSave(onStepSave);
    ui::ViewStack::instance().push(&s_t9Input);
}

/**
 * \brief Persists wizard data and returns to the anchor view on success.
 */
static void wizardFinish() {
    char buf[VCARD_MAX_LEN + 1];
    size_t len = vcard_generate_from_struct(&s_wizard.data, buf, sizeof(buf));
    if (len == 0) {
        ui::showToastError(ui::tr(s_failedKey ? s_failedKey : "core.failed"));
        s_wizard.active = false;
        return;
    }

    char err[64] = {};
    bool ok = false;
    switch (s_wizard.target) {
        case WizardTarget::OWN:
            ok = vcard_store_set_own(buf, len, err, sizeof(err));
            break;
        case WizardTarget::RECEIVED_NEW:
            ok = vcard_store_add(buf, len, err, sizeof(err));
            break;
        case WizardTarget::RECEIVED_EDIT:
            ok = vcard_store_update(s_wizard.editSlot, buf, len, err, sizeof(err));
            break;
    }
    if (!ok) {
        LOG_W(TAG, "save failed: %s", err[0] ? err : "(no detail)");
        ui::showToastError(err[0] ? err
                                  : ui::tr(s_failedKey ? s_failedKey : "core.failed"));
        s_wizard.active = false;
        return;
    }

    ui::showToastSuccess(ui::tr(s_savedKey ? s_savedKey : "core.saved"));
    VcardWizard::DoneCallback done = s_wizard.onDone;
    ui::IView* anchor = s_wizard.returnAnchor;
    s_wizard.active = false;
    // Refresh the caller's list before navigating back so it reflects the save.
    if (done) done();
    if (anchor) {
        ui::ViewStack::instance().popToAnchor(anchor);
    }
}

/**
 * \brief Per-step save handler. Copies the entered text into the matching struct
 *        field, advances to the next step, and finishes the wizard after the last step.
 */
static void onStepSave(const char* text) {
    if (!s_wizard.active || s_wizard.currentStep >= STEP_COUNT) return;

    const FieldRef& ref = k_steps[s_wizard.currentStep];
    char* field = ref.get(&s_wizard.data);
    size_t cap = ref.maxLen;
    if (text) {
        size_t len = strlen(text);
        if (len > cap) len = cap;
        memcpy(field, text, len);
        field[len] = '\0';
    } else {
        field[0] = '\0';
    }

    s_wizard.currentStep++;
    if (s_wizard.currentStep >= STEP_COUNT) {
        wizardFinish();
    } else {
        pushCurrentStep();
    }
}

void VcardWizard::configure(const char* const* titleKeys, const char* savedKey,
                            const char* failedKey) {
    s_titleKeys = titleKeys;
    s_savedKey = savedKey;
    s_failedKey = failedKey;
}

void VcardWizard::start(ui::IView* returnAnchor) {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.returnAnchor = returnAnchor;
    s_wizard.active = true;
    s_wizard.currentStep = 0;
    pushCurrentStep();
}

void VcardWizard::edit(ui::IView* returnAnchor) {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.returnAnchor = returnAnchor;
    s_wizard.active = true;
    s_wizard.currentStep = 0;

    char raw[VCARD_MAX_LEN + 1];
    size_t got = vcard_store_get_own(raw, sizeof(raw));
    if (got > 0) {
        vcard_parse_to_struct(raw, &s_wizard.data);
    }
    pushCurrentStep();
}

void VcardWizard::startReceived(ui::IView* returnAnchor, DoneCallback onDone) {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.returnAnchor = returnAnchor;
    s_wizard.active = true;
    s_wizard.currentStep = 0;
    s_wizard.target = WizardTarget::RECEIVED_NEW;
    s_wizard.onDone = onDone;
    pushCurrentStep();
}

void VcardWizard::editReceived(ui::IView* returnAnchor, uint16_t slot, DoneCallback onDone) {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.returnAnchor = returnAnchor;
    s_wizard.active = true;
    s_wizard.currentStep = 0;
    s_wizard.target = WizardTarget::RECEIVED_EDIT;
    s_wizard.editSlot = slot;
    s_wizard.onDone = onDone;

    char raw[VCARD_MAX_LEN + 1];
    if (vcard_store_get(slot, raw, sizeof(raw)) > 0) {
        vcard_parse_to_struct(raw, &s_wizard.data);
    }
    pushCurrentStep();
}

} // namespace cdc::mod_vcard
