/**
 * Feature Flags - Compile-time feature toggles
 *
 * Set to 0 to completely disable a feature (zero overhead).
 * When disabled, related code is not compiled at all.
 */

#pragma once

#include "sdkconfig.h"

// ============================================================================
// Security Features
// ============================================================================

// Secure Serial (require PIN for serial commands)
// Maps from Kconfig CONFIG_SECURE_SERIAL
#ifdef CONFIG_SECURE_SERIAL
#define FEATURE_SECURE_SERIAL 1
#else
#ifndef FEATURE_SECURE_SERIAL
#define FEATURE_SECURE_SERIAL 0
#endif
#endif

// NVS Editor destructive actions (privileged tool)
#ifndef FEATURE_NVS_EDIT
#define FEATURE_NVS_EDIT 0
#endif

// Plugin AOT (ahead-of-time native code). Default off: AOT artifacts run as
// native machine code and bypass the WASM bounds-checked sandbox, so only
// interpreted bytecode is loaded/accepted unless this is explicitly enabled.
#ifndef FEATURE_PLUGIN_AOT
#define FEATURE_PLUGIN_AOT 0
#endif

// Lock the on-device system folder (plugins, i18n) read-only/hidden/system at
// mount. Default off: an opt-in security feature so a USB-MSC host cannot modify
// or delete system files. Off by default so plugin upload/overwrite/delete over
// serial work normally without read-only attributes blocking them.
#ifndef FEATURE_PLUGIN_SYSTEM_LOCK
#define FEATURE_PLUGIN_SYSTEM_LOCK 0
#endif

// ============================================================================
// Content Viewers (image decode + Markdown rendering)
// ============================================================================

// Image viewer decoders. Each format is gated independently so an over-budget
// decoder can be dropped without code changes (flash measured via idf.py size).
#ifndef FEATURE_IMG_JPEG
#define FEATURE_IMG_JPEG 1
#endif
#ifndef FEATURE_IMG_PNG
#define FEATURE_IMG_PNG 1
#endif

// Markdown rendering in the scrollable text viewer.
#ifndef FEATURE_MARKDOWN
#define FEATURE_MARKDOWN 1
#endif

// Debug Mode (disables lockouts, useful for development)
#ifndef DEBUG_MODE
#define DEBUG_MODE 1
#endif

// Build profile byte. A mismatch between the byte stored in NVS and the
// byte compiled into the running firmware triggers a complete factory wipe
// (NVS partition + TROPIC01 R-Memory + ECC slots) at the next boot. This
// is the beta-phase software guard; bypass-resistant enforcement against an
// active attacker requires Secure Boot v2 with anti-rollback and is on the
// 1.0 roadmap (see website/src/content/docs/security/caveats.md).
#define BUILD_PROFILE_BYTE \
    ((FEATURE_SECURE_SERIAL ? 0x02 : 0x00) | (DEBUG_MODE ? 0x01 : 0x00))
