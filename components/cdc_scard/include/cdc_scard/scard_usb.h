/*
 * Refcounted ownership of the shared USB CCID interface.
 *
 * Multiple applet modules (OpenPGP, PIV, OATH) front the single CCID
 * interface. The first acquire brings the interface up (ccid_init +
 * UsbManager registration under the owner name "cdc_scard"); the last
 * release tears it down. A user-facing service gate (scard_usb_set_enabled)
 * can keep the interface out of the descriptor while applet modules stay
 * registered, so re-enabling needs no module restart. Not thread-safe:
 * module start/stop runs on the main task only.
 */

#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Acquire a reference on the shared CCID USB interface. With the gate open,
// returns false when the interface cannot be enumerated (USB endpoint budget
// exhausted, USB unavailable); the refcount is not incremented in that case
// and the caller must skip its applet registration. With the gate closed the
// reference is counted and true is returned without touching the descriptor;
// the interface appears once the gate opens.
bool scard_usb_acquire(void);

// Drop a reference. The last release unregisters the CCID interface (when it
// is part of the descriptor). Releases without a matching successful acquire
// are ignored.
void scard_usb_release(void);

// Service gate: false keeps the CCID interface out of the USB descriptor
// regardless of the refcount. Enabling with references held registers the
// interface immediately; returns false when the endpoint budget refuses it.
bool scard_usb_set_enabled(bool on);
bool scard_usb_enabled(void);

// True while at least one applet module holds a reference.
bool scard_usb_in_use(void);

#ifdef __cplusplus
}
#endif
