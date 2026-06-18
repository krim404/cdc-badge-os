#include "BrowserPageView.h"

#include "BrowserController.h"

#include <cstdio>
#include <cstring>

namespace cdc::browser {

using cdc::ui::InputResult;

void BrowserPageView::setUrlFooter()
{
    const size_t cap = sizeof(footer_);
    size_t n = std::strlen(url_);
    if (n < cap) {
        std::memcpy(footer_, url_, n);
        footer_[n] = '\0';
    } else {
        std::memcpy(footer_, url_, cap - 3);
        footer_[cap - 3] = '.';
        footer_[cap - 2] = '.';
        footer_[cap - 1] = '\0';
    }
}

void BrowserPageView::loadDoc(const char* title, const char* src, size_t len, const char* url)
{
    cdc::ui::MarkdownView::init(title, src, len);  // calls rebuildInteractive()
    std::strncpy(url_, url ? url : "", sizeof(url_) - 1);
    url_[sizeof(url_) - 1] = '\0';
    setUrlFooter();
}

void BrowserPageView::rebuildInteractive()
{
    // Base finds the inline content links (delimited \x10 markers from the
    // extractor) and any task-list checkboxes, in document order.
    cdc::ui::MarkdownView::rebuildInteractive();

    // Append the page's form controls: "[>]" submit, "[ ]"/"[x]" checkboxes (in
    // field order). These render at the end of the body as their own lines.
    uint16_t checkOrd = 0;
    for (uint16_t i = 0; i < mdRowCount(); ++i) {
        const char* t = nullptr;
        uint16_t len = 0;
        bool first = false;
        uint32_t off = 0;
        mdRowInfo(i, t, len, first, off);
        if (first && len >= 3 && t[0] == '[' && t[2] == ']') {
            if (t[1] == '>') addInteract(i, Interact::Submit, 0, 0);
            else if (t[1] == ' ' || t[1] == 'x' || t[1] == 'X')
                addInteract(i, Interact::Check, checkOrd++, off + 1);
        }
    }
}

void BrowserPageView::activateSelected()
{
    const InteractRef* it = selectedRef();
    if (!it) return;
    switch (it->kind) {
        case Interact::Link:
            browserFollowLink(it->id);
            break;
        case Interact::Check: {
            bool nowChecked = flipCheckChar(it->srcOff);
            browserSetFormCheckbox(it->id, nowChecked);
            markDirty();
            break;
        }
        case Interact::Submit:
            browserSubmitForm();
            break;
        case Interact::Image:  // browser pages do not emit image markers
            break;
    }
}

void BrowserPageView::updateFooterForSelection()
{
    const InteractRef* it = selectedRef();
    if (it && it->kind == Interact::Link) {
        char label[80] = {0};
        char href[256] = {0};
        browserLinkInfo(it->id, label, sizeof(label), href, sizeof(href));
        std::snprintf(footer_, sizeof(footer_), "[%u] %.46s", static_cast<unsigned>(it->id + 1),
                      label[0] ? label : href);
    } else {
        setUrlFooter();
    }
    markDirty();
}

InputResult BrowserPageView::onKey(char key)
{
    switch (key) {
        case '6':
            cycleInteractive(+1);
            updateFooterForSelection();
            return InputResult::CONSUMED;
        case '4':
            cycleInteractive(-1);
            updateFooterForSelection();
            return InputResult::CONSUMED;
        case 'Y':
            if (selectedRef()) activateSelected();
            else browserOpenLinks();  // nothing selected -> the full links list
            return InputResult::CONSUMED;
        case '3':
            browserOpenPageMenu();
            return InputResult::CONSUMED;
        case 'N':
            if (browserHistoryBack()) return InputResult::CONSUMED;
            return InputResult::REQUEST_POP;
        default:
            return cdc::ui::MarkdownView::onKey(key);  // 2/8 scroll etc.
    }
}

InputResult BrowserPageView::onLongPress(char key)
{
    if (key == 'N') {
        exitBrowser();
        return InputResult::CONSUMED;
    }
    return cdc::ui::MarkdownView::onLongPress(key);
}

}  // namespace cdc::browser
