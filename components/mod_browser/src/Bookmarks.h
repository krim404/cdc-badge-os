#pragma once

#include "BrowserExtractTypes.h"

#include <cstddef>
#include <cstdint>

/**
 * \file Bookmarks.h
 * \brief Persistent (NVS) bookmark store for the browser.
 */

namespace cdc::browser {

constexpr uint8_t kMaxBookmarks = 16;

struct Bookmark {
    char url[kUrlCap]     = {0};
    char label[kLabelCap] = {0};  ///< CP437 display label (page title or URL).
};

/**
 * \brief Singleton bookmark list backed by NVS (namespace "browser", blob "bm").
 *
 * Labels are stored in the display codepage (CP437). The on-NVS blob is validated
 * by magic; a mismatch reinitialises to an empty list (no migration).
 */
class Bookmarks {
public:
    static Bookmarks& instance();

    /// \brief Load from NVS. Call once at module init.
    void load();

    uint8_t count() const { return count_; }
    const Bookmark& at(uint8_t i) const { return items_[i]; }

    /// \brief Add (deduped, newest first) and persist. \return false if full/invalid.
    bool add(const char* url, const char* label);
    /// \brief Remove the entry at \p index and persist.
    bool removeAt(uint8_t index);
    /// \brief True if \p url is already bookmarked.
    bool contains(const char* url) const;

private:
    Bookmarks() = default;
    void save() const;

    Bookmark items_[kMaxBookmarks];
    uint8_t  count_ = 0;
};

}  // namespace cdc::browser
