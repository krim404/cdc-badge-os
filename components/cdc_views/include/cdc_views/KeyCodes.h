#pragma once

/**
 * \brief Centralized key-code constants for cdc_views.
 *
 * The badge uses a TCA9535-driven 12-key keypad with the following
 * physical layout:
 *
 *     1 2 3
 *     4 5 6
 *     7 8 9
 *     N 0 Y
 *
 * Across the views these characters carry conventional UI meanings.
 * Using named constants keeps key handlers readable and makes it
 * easier to retarget the badge to a different keypad in the future.
 *
 * Notes:
 *  - 'Y' (yes/OK) and 'N' (no/cancel) are shared by every view.
 *  - 'N' also acts as backspace in text-entry views.
 *  - '2' / '8' are used for vertical navigation in list-style views.
 *  - '3' opens a context menu where one is provided.
 *  - '5' is the digit "five"; some views also use it as a center-select.
 *
 * Not every view consumes every key; views that do not need a given
 * action simply ignore it.
 */

namespace cdc::ui {

/** \brief Move selection up (numeric '2'). */
static constexpr char KEY_UP = '2';

/** \brief Move selection down (numeric '8'). */
static constexpr char KEY_DOWN = '8';

/** \brief Center select / digit '5'. */
static constexpr char KEY_SELECT = '5';

/** \brief Confirm / OK / Save. */
static constexpr char KEY_YES = 'Y';

/** \brief Cancel / Back / Backspace. */
static constexpr char KEY_NO = 'N';

/** \brief Open context menu / digit '3'. */
static constexpr char KEY_MENU = '3';

} // namespace cdc::ui
