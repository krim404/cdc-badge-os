/*
 * ISO 7816 applet registry and APDU dispatcher for CDC Badge.
 *
 * Multiple card applications (OpenPGP, PIV, OATH, ...) share the single
 * USB CCID interface. The dispatcher intercepts SELECT-by-AID commands,
 * routes them to the matching registered applet, and forwards every other
 * APDU to the currently selected applet (or the default applet when no
 * SELECT happened yet).
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of applets that can be registered at once.
#define SCARD_MAX_APPLETS 4

typedef struct {
    const char *name;              // Unique applet identifier, e.g. "openpgp"
    const uint8_t *aid;            // Registered AID prefix
    uint8_t aid_len;               // Bytes compared against SELECT data
    int  (*process_apdu)(const uint8_t *cmd, size_t cmd_len,
                         uint8_t *resp, size_t resp_max);
    void (*deselect)(void);        // Wipe PIN/session/chaining state
} scard_applet_t;

// Register an applet. The struct is copied; aid/name must stay valid for
// the applet's lifetime. make_default routes pre-SELECT traffic to it.
bool scard_register_applet(const scard_applet_t *applet, bool make_default);

// Remove a previously registered applet by name.
void scard_unregister_applet(const char *name);

// Route one APDU to the appropriate applet (see file header for semantics).
// Returns the response length including SW1-SW2.
int scard_dispatch_apdu(const uint8_t *cmd, size_t cmd_len,
                        uint8_t *resp, size_t resp_max);

// Card-level reset: deselect the current applet and clear the selection.
// Called on CCID ICC power-on/off and USB bus reset.
void scard_reset(void);

#ifdef __cplusplus
}
#endif
