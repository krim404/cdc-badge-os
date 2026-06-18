#pragma once

#include <cstddef>
#include <cstdint>

#include "cdc_core/Raii.h"
#include "cdc_ui/IView.h"
#include "cdc_views/MarkdownModel.h"

namespace cdc::ui {

/**
 * \brief Scrollable viewer that renders a Markdown source.
 *
 * Parses the source into styled lines (\ref parseMarkdown), wraps them into
 * per-font visual rows, and scrolls them with variable line heights. Headings
 * use the larger bold fonts; lists, block quotes, code and rules are visually
 * distinct. All buffers live in PSRAM and are released on exit.
 *
 * Keys: 2 = up, 8 = down, N = back.
 */
class MarkdownView : public ViewBase {
public:
    static constexpr uint32_t MAX_SOURCE = 64u * 1024u;  ///< Source byte cap.
    static constexpr uint16_t MAX_ROWS = 600;            ///< Visual-row cap.

    /**
     * \brief Loads and lays out a Markdown source.
     * \param title Header title (CP437).
     * \param src Markdown source (CP437; ASCII markup is codepage-independent).
     * \param len Source length in bytes.
     */
    void init(const char* title, const char* src, size_t len);

    /**
     * \brief Loads a source as plain, unrendered text (no Markdown parsing).
     *
     * Splits on newlines and word-wraps each line with the built-in font; inline
     * emphasis markers are shown verbatim. Used for the browser's source view.
     * \param title Header title (CP437).
     * \param src Text (CP437).
     * \param len Source length in bytes.
     */
    void initPlain(const char* title, const char* src, size_t len);

    /// \brief Sink callback: wraps one parsed styled line into visual rows.
    void appendParsed(const StyledLine& line);

    void render(bool partial) override;
    InputResult onKey(char key) override;
    InputResult onLongPress(char key) override;
    void onExit() override;
    const char* getName() const override { return "MarkdownView"; }
    const char* getFooterHint() const override;

    /**
     * \brief Highlight the row containing the inline marker "[linkNum]" and scroll
     *        it into view. Pass 0 to clear the highlight. Used for in-view link
     *        selection by subclasses; the base renders the highlighted row inverted.
     */
    void selectMarker(uint16_t linkNum);

    /// \brief Interactive element kinds the view can select (4/6) and activate (Y).
    enum class Interact : uint8_t { Link, Check, Submit, Image };

    /// \brief One selectable interactive element discovered in the rendered rows.
    struct InteractRef {
        uint16_t row;     ///< Visual row index (highlight/scroll target).
        Interact kind;
        uint16_t id;      ///< Link: 1-based marker number; Check/Submit: ordinal.
        uint32_t srcOff;  ///< Check: byte offset of the state char in the source; else 0.
    };

    /// \brief Move the interactive selection by \p dir (+1/-1), highlighting its row.
    void cycleInteractive(int dir);

    /// \brief Clear the current interactive selection.
    void clearSelection() { interactSel_ = -1; highlightRow_ = -1; dirty_ = true; }

    /// \brief Number of interactive elements on the page.
    uint16_t interactiveCount() const { return interactCount_; }

    /**
     * \brief Persistence callback fired after a task-list checkbox toggles.
     *        Receives the full modified CP437 source so the owner can write it
     *        back (e.g. to the file). Leave unset for read-only documents.
     */
    using CheckSaveFn = void (*)(void* ud, const char* cp437Source, size_t len);
    void setCheckSave(CheckSaveFn fn, void* ud) { checkSave_ = fn; checkSaveUd_ = ud; }

protected:
    /// \brief Rebuild the interactive-element list from the rendered rows.
    /// Base finds task-list checkboxes; subclasses extend with links/forms.
    virtual void rebuildInteractive();

    /// \brief Activate the selected element (Y). Base toggles checkboxes.
    virtual void activateSelected();

    // Helpers for subclasses (avoid exposing the private Row type).
    uint16_t mdRowCount() const { return rowCount_; }
    void     mdRowInfo(uint16_t i, const char*& text, uint16_t& len, bool& first,
                       uint32_t& srcOff) const;
    void     addInteract(uint16_t row, Interact kind, uint16_t id, uint32_t srcOff);
    void     clearInteract() { interactCount_ = 0; interactSel_ = -1; }
    const InteractRef* selectedRef() const;
    /// \brief Toggle a checkbox state char in the source. \return the new checked state.
    bool     flipCheckChar(uint32_t srcOff);
    /// \brief Run the persistence callback with the current source (if set).
    void     fireCheckSave();

private:
    static constexpr uint16_t MAX_TITLE_LEN = 64;
    static constexpr uint16_t MAX_INTERACT = 256;
    static constexpr size_t   kLinkPoolCap = 8192;  ///< Packed inline-link URL pool.

    struct Row {
        const char* text;  ///< Span into srcBuf_ (not null-terminated).
        uint16_t len;
        uint8_t font;    ///< One of \ref mdfont.
        uint8_t indent;  ///< Nesting depth.
        MdLineKind kind;
        bool inverted;
        bool first;  ///< First visual row of its logical line (draws the marker).
    };

    char titleBuf_[MAX_TITLE_LEN];
    cdc::core::PsramUniquePtr<char> srcBuf_;
    cdc::core::PsramUniquePtr<Row> rows_;
    uint16_t rowCount_ = 0;
    uint16_t scrollRow_ = 0;
    bool truncated_ = false;
    bool plain_ = false;     ///< Plain mode: rows are verbatim, no inline-marker stripping.
    int textAreaWidth_ = 0;
    int highlightRow_ = -1;  ///< Row drawn inverted for in-view marker selection.

    cdc::core::PsramUniquePtr<InteractRef> interact_;
    uint16_t interactCount_ = 0;
    int interactSel_ = -1;
    CheckSaveFn checkSave_ = nullptr;
    void* checkSaveUd_ = nullptr;

    // Markdown inline-link targets: the rewrite replaces "[text](url)" with the
    // delimited form "\x10text\x11" and records each url here, indexed by the
    // delimiter's document order (matches the ordinal scanned in rebuildInteractive).
    cdc::core::PsramUniquePtr<char>     linkPool_;     ///< Packed NUL-terminated URLs.
    cdc::core::PsramUniquePtr<uint16_t> linkUrlOff_;   ///< ordinal -> offset into linkPool_.
    uint16_t linkPoolLen_ = 0;
    uint16_t inlineLinkCount_ = 0;

    void addRow(const char* text, uint16_t len, uint8_t font, uint8_t indent,
                MdLineKind kind, bool inverted, bool first);
    uint16_t visibleRowCount() const;
    void rewriteInlineLinks();  ///< "[text](url)" -> "\x10text\x11" + url pool.
    void drawStyledRow(void* gfx, const char* text, uint16_t len, int x, int yTop,
                       uint16_t fg);  ///< Built-in-font row with inline styling.
};

/**
 * \brief Parses and shows a Markdown source on the view stack.
 * \param title Header title.
 * \param src Markdown source.
 * \param len Source length in bytes.
 * \return Pointer to the shared MarkdownView instance.
 */
MarkdownView* showMarkdown(const char* title, const char* src, size_t len);

/**
 * \brief Shows a source as plain, unrendered text on the view stack.
 * \param title Header title.
 * \param src Text (CP437).
 * \param len Source length in bytes.
 * \return Pointer to the shared MarkdownView instance.
 */
MarkdownView* showPlainText(const char* title, const char* src, size_t len);

} // namespace cdc::ui
