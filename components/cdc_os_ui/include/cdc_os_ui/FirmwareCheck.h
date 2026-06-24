#pragma once

#include <cstddef>

/**
 * \file
 * \brief Expert-menu firmware/version screen with an optional upstream update
 *        check against the GitHub releases of cdc-badge-os.
 */

namespace cdc::ui {

/**
 * \brief Opens the Firmware Check screen and pushes it onto the view stack.
 *
 * Shows the running firmware version and the plugin host API level. When WiFi
 * is already connected it asynchronously queries the upstream GitHub release
 * and reports whether a newer version is available. Pressing the confirm key
 * re-checks; if WiFi is not connected, the confirm key first brings it up using
 * the saved network, then checks.
 */
void showFirmwareCheck();

/**
 * \brief Formats the last persisted upstream check into \p out.
 *
 * Reads the latest upstream release tag and the check date stored in NVS by the
 * Firmware Check screen and renders them as e.g. "v0.7.3 (checked: 24.06.26)".
 *
 * \param out Destination buffer.
 * \param cap Size of \p out.
 * \return true if a stored result was found and written, false otherwise.
 */
bool firmwareCheckLastResult(char* out, size_t cap);

} // namespace cdc::ui
