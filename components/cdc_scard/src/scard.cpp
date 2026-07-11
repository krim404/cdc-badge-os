/**
 * \brief ISO 7816 applet registry and APDU dispatcher.
 *
 * Routes SELECT-by-AID commands to the matching registered applet and
 * forwards all other traffic to the currently selected applet. The
 * dispatcher never invents status words for APDUs an applet can answer
 * itself: pre-SELECT commands go to the default applet so its own state
 * gates (e.g. OpenPGP's app_selected check) produce the exact same
 * responses as the previous single-applet wiring.
 */

#include "cdc_scard/applet.h"
#include "cdc_scard/apdu.h"
#include "cdc_log.h"
#include <string.h>

static const char *TAG = "SCARD";

// ISO 7816-4: file or application not found.
#define SCARD_SW_FILE_NOT_FOUND 0x6A82

// SELECT P1 = 0x04: select by DF name (AID).
#define SCARD_SELECT_BY_DF_NAME 0x04

static scard_applet_t s_applets[SCARD_MAX_APPLETS];
static uint8_t s_count = 0;
static int s_current = -1;
static int s_default = -1;

/**
 * \brief Registers an applet in the dispatch table.
 * \param applet Applet descriptor (copied; aid/name storage must outlive it).
 * \param make_default Route pre-SELECT APDUs to this applet.
 * \return `true` if the applet was registered.
 */
bool scard_register_applet(const scard_applet_t *applet, bool make_default) {
    if (!applet || !applet->name || !applet->aid || applet->aid_len == 0 ||
        !applet->process_apdu) {
        LOG_E(TAG, "Invalid applet descriptor");
        return false;
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (strcmp(s_applets[i].name, applet->name) == 0) {
            LOG_E(TAG, "Applet '%s' already registered", applet->name);
            return false;
        }
    }
    if (s_count >= SCARD_MAX_APPLETS) {
        LOG_E(TAG, "Applet table full (%d)", SCARD_MAX_APPLETS);
        return false;
    }
    s_applets[s_count] = *applet;
    if (make_default) {
        s_default = s_count;
    }
    s_count++;
    LOG_I(TAG, "Applet '%s' registered (aid_len=%u%s)", applet->name,
          applet->aid_len, make_default ? ", default" : "");
    return true;
}

/**
 * \brief Removes an applet from the dispatch table.
 * \param name Applet identifier used at registration.
 */
void scard_unregister_applet(const char *name) {
    if (!name) {
        return;
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (strcmp(s_applets[i].name, name) != 0) {
            continue;
        }
        if (s_current == i) {
            if (s_applets[i].deselect) {
                s_applets[i].deselect();
            }
            s_current = -1;
        }
        if (s_default == i) {
            s_default = -1;
        }
        // Compact the table and fix indices pointing past the removed slot.
        for (uint8_t j = i; j + 1 < s_count; j++) {
            s_applets[j] = s_applets[j + 1];
        }
        s_count--;
        if (s_current > i) s_current--;
        if (s_default > i) s_default--;
        LOG_I(TAG, "Applet '%s' unregistered", name);
        return;
    }
}

/**
 * \brief Finds the registered applet whose AID prefix matches the SELECT data.
 * \return Table index of the longest-prefix match, or -1 when nothing matches.
 */
static int find_applet_by_aid(const uint8_t *data, uint16_t len) {
    int best = -1;
    uint8_t best_len = 0;
    for (uint8_t i = 0; i < s_count; i++) {
        const scard_applet_t *a = &s_applets[i];
        if (len >= a->aid_len && memcmp(data, a->aid, a->aid_len) == 0 &&
            a->aid_len > best_len) {
            best = i;
            best_len = a->aid_len;
        }
    }
    return best;
}

/**
 * \brief Routes one APDU to the appropriate applet.
 * \param cmd Raw APDU bytes.
 * \param cmd_len Length of `cmd`.
 * \param resp Output response buffer.
 * \param resp_max Capacity of `resp`.
 * \return Response length including SW1-SW2.
 */
int scard_dispatch_apdu(const uint8_t *cmd, size_t cmd_len,
                        uint8_t *resp, size_t resp_max) {
    if (!resp || resp_max < 2) {
        return -1;
    }
    if (s_count == 0) {
        return (int)apdu_sw(resp, SCARD_SW_FILE_NOT_FOUND);
    }

    // Only plain SELECT-by-DF-name is dispatcher business. Chain blocks
    // (CLA bit 0x10) and every other command pass through untouched so the
    // applets keep full control over their protocol state machines.
    apdu_t apdu;
    if (apdu_parse(cmd, cmd_len, &apdu) && apdu.cla == CLA_ISO7816 &&
        apdu.ins == INS_SELECT && apdu.p1 == SCARD_SELECT_BY_DF_NAME) {
        int match = find_applet_by_aid(apdu.data, apdu.lc);
        if (match >= 0) {
            if (s_current >= 0 && s_current != match &&
                s_applets[s_current].deselect) {
                s_applets[s_current].deselect();
            }
            if (s_current != match) {
                LOG_I(TAG, "Applet '%s' selected", s_applets[match].name);
            }
            s_current = match;
            // Forward the SELECT so the applet emits its own FCI/SW and
            // performs its select-time state reset.
            return s_applets[match].process_apdu(cmd, cmd_len, resp, resp_max);
        }
        if (s_current >= 0) {
            // Unknown AID: the selected applet answers (and wipes its
            // session state), matching the legacy single-applet behavior.
            return s_applets[s_current].process_apdu(cmd, cmd_len, resp, resp_max);
        }
        return (int)apdu_sw(resp, SCARD_SW_FILE_NOT_FOUND);
    }

    int target = (s_current >= 0) ? s_current : s_default;
    if (target < 0) {
        return (int)apdu_sw(resp, SCARD_SW_FILE_NOT_FOUND);
    }
    return s_applets[target].process_apdu(cmd, cmd_len, resp, resp_max);
}

/**
 * \brief Card-level reset: deselects the current applet.
 *
 * Called on CCID ICC power-on/off and USB bus reset so PIN verification
 * and session state never survive a (virtual) card power cycle.
 */
void scard_reset(void) {
    if (s_current >= 0 && s_applets[s_current].deselect) {
        s_applets[s_current].deselect();
    }
    s_current = -1;
}
