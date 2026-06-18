#pragma once

#include "cdc_views/MarkdownView.h"

/**
 * \file BrowserPageView.h
 * \brief Page renderer: a MarkdownView with browser navigation keys.
 *
 * Keys: 2/8 scroll (inherited), 4/6 select the previous/next interactive element
 * (link, form checkbox, submit), Y activates it (follow / toggle / submit) or,
 * with nothing selected, opens the full links list. 3 = context menu, N (short)
 * = history back (or pop to bookmarks), N (long) = exit browser. The footer shows
 * the current URL, or the selected link's target.
 */

namespace cdc::browser {

class BrowserPageView : public cdc::ui::MarkdownView {
public:
    /// \brief Render a CP437 source as the current page and set the footer URL.
    void loadDoc(const char* title, const char* src, size_t len, const char* url);

    cdc::ui::InputResult onKey(char key) override;
    cdc::ui::InputResult onLongPress(char key) override;
    const char* getFooterHint() const override { return footer_; }
    const char* getName() const override { return "BrowserPageView"; }

protected:
    void rebuildInteractive() override;  ///< Links + form checkboxes/submit, in document order.
    void activateSelected() override;    ///< Follow link / toggle checkbox / submit form.

private:
    void setUrlFooter();              ///< Restore the footer to the page URL.
    void updateFooterForSelection();  ///< Footer shows the selected link's target.

    char footer_[96] = {0};
    char url_[256] = {0};            ///< Current page URL (for footer + selection reset).
};

}  // namespace cdc::browser
