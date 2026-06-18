#pragma once

#include "cdc_ui/IView.h"

#include <cstddef>

/**
 * \file BrowserController.h
 * \brief Coordinator hooks for the browser flow (entry, navigation, menus).
 *
 * Implemented in BrowserController.cpp over static view instances and PSRAM
 * working buffers. The page view (BrowserPageView) and the module call these.
 */

namespace cdc::browser {

/// \brief Build and return the browser home view (bookmarks + "Open webpage").
cdc::ui::IView* browserEntryView();

/// \brief Enter the browser and load \p url (registered as the cdc::ui URL opener).
/// \p url is UTF-8.
void browserOpenUrl(const char* url);

/// \brief Open the in-page links list (Y on a page).
void browserOpenLinks();

/// \brief Open the page context menu (3 on a page).
void browserOpenPageMenu();

/// \brief Navigate one step back in history. \return true if it navigated.
bool browserHistoryBack();

/// \brief Number of links collected on the current page.
uint16_t browserLinkCount();

/// \brief Fill CP437 label and the raw href for link \p idx. \return false if out of range.
bool browserLinkInfo(uint16_t idx, char* labelOut, size_t labelCap, char* hrefOut, size_t hrefCap);

/// \brief Load the target of link \p idx (in-view selection follow).
void browserFollowLink(uint16_t idx);

/// \brief Set the checked state of the \p ordinal-th checkbox field of the current form.
void browserSetFormCheckbox(uint16_t ordinal, bool checked);

/// \brief Submit the current page's form (POST/GET the built body).
void browserSubmitForm();

/// \brief Leave the browser, returning to the Tools menu.
void exitBrowser();

/// \brief Poll the async fetch worker; completes a finished load. Call from the UI tick.
void browserPoll();

/// \brief Render an in-memory HTML document (e.g. a local file) in a scrollable view.
/// \p title and \p html are UTF-8. Registered as the cdc::ui HTML viewer hook.
void browserShowLocalHtml(const char* title, const char* html, size_t len);

}  // namespace cdc::browser
