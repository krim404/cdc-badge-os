/**
 * \file I18n.h
 * \brief Internationalization with English fallbacks in code and overlay
 *        translations loaded at runtime from a VFAT JSON file.
 *
 * Architecture:
 *  - English fallbacks live in code as `I18nEntry` tables registered by each
 *    module at startup. They sit in rodata and are always available.
 *  - Every other language lives in its own flat file
 *    `/plugins/i18n/lang_<code>.json` on the plugins FAT partition, e.g.
 *    `lang_de.json`. The file is a flat `{ "<key>": "<value>", ... }` object
 *    and its `core.lang_name` value is the language's own display name
 *    (endonym). Adding a language is just dropping a new `lang_<code>.json`;
 *    it appears in the picker automatically.
 *  - The active language's file is parsed into a PSRAM-backed key-value table;
 *    the set of selectable languages is discovered by scanning the directory.
 *  - Plugin manifest strings keep their own `i18n_strings` map and are
 *    queried via `host_i18n_tr_key` - unchanged by this rewrite.
 *
 * Key conventions:
 *  - `core.*`         for firmware-wide strings.
 *  - `mod_<name>.*`   for module-specific strings.
 *  - Keys are 7-bit ASCII, snake_case, no spaces.
 */

#pragma once

#include "cdc_core/Raii.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cdc::ui {

/**
 * \brief Single English translation entry.
 *
 * Both fields point to rodata literals. Modules declare static constexpr
 * arrays of these and register them with `I18n::registerEnglishTable()`.
 */
struct I18nEntry {
    const char* key;   ///< Stable string key, e.g. "core.save" or "mod_2fa.codes".
    const char* en;    ///< English translation - rodata literal.
};

/**
 * \brief Internationalization singleton.
 *
 * Lookup order for `tr(key)`:
 *   1. If current language is "en" or no overlay loaded: return English fallback.
 *   2. If overlay has a translation for the current language and key: return it.
 *   3. Else: fall back to English. Returns "?<key>" if even English is missing.
 *
 * Thread safety: registration is expected to happen single-threaded during
 * module init. `tr()` is read-only after all modules have registered.
 */
/// One selectable overlay language: ISO code plus its own display name.
struct OverlayLanguage {
    std::string code;   ///< Language code, e.g. "de" (lower-case).
    std::string name;   ///< Endonym for the picker (CP437-encoded), e.g. "Deutsch".
};

class I18n {
public:
    /// Directory on the plugins FAT holding the per-language files.
    static constexpr const char* OVERLAY_DIR = "/plugins/i18n";

    /// Singleton accessor.
    static I18n& instance();

    /**
     * \brief Initialize and load persisted language code from NVS.
     * \return true on success.
     */
    bool init();

    /**
     * \brief Rescan available languages and (re)load the active overlay.
     *
     * Scans `OVERLAY_DIR` for `lang_<code>.json` files to populate the picker
     * list, then parses the file for the current language into the active
     * table. A `lang_<code>.json` is a flat object:
     * \code
     * { "core.lang_name": "Deutsch", "core.save": "Speichern", ... }
     * \endcode
     * Safe to call when the current language is "en" (just rescans).
     *
     * \return true if the active overlay loaded (or language is "en"), false
     *         if the active language's file was missing/invalid.
     */
    bool loadOverlay();

    /**
     * \brief Append English entries to the lookup table.
     *
     * Typically called once per module from its `init()`. Entries are stored
     * by-pointer (no copy); the caller must keep the array alive for the
     * lifetime of the firmware (rodata is fine).
     *
     * \param entries Pointer to the first entry.
     * \param count   Number of entries.
     */
    void registerEnglishTable(const I18nEntry* entries, std::size_t count);

    /**
     * \brief Look up a translation by key.
     * \param key Stable string key. Must not be null.
     * \return Pointer to translation text. Never null - returns "?<key>" if no match.
     */
    const char* tr(const char* key) const;

    /**
     * \brief Overlay-only lookup with no English fallback.
     *
     * Returns the translation from the currently active overlay language, or
     * `nullptr` if the key is missing OR the active language is "en". Useful
     * for plugin code that wants to try a namespaced key in the central
     * overlay before falling back to its own English manifest table.
     *
     * \param key Stable string key.
     * \return Pointer into PSRAM overlay storage (stable until the active
     *         language changes), or `nullptr`.
     */
    const char* overlayTr(const char* key) const;

    /// Current language code (lower-case ISO-639-1, e.g. "en", "de").
    const std::string& getLanguageCode() const { return currentLang_; }

    /**
     * \brief Set the active language by code.
     *
     * If the overlay does not contain the requested language, the call still
     * succeeds and persists the choice, but `tr()` will fall back to English
     * until an overlay covering the language is loaded.
     *
     * \param code Language code (e.g. "en", "de"). Empty defaults to "en".
     * \return true on success.
     */
    bool setLanguageCode(const char* code);

    /// Languages discovered on the plugins FAT (does not include "en").
    const std::vector<OverlayLanguage>& availableOverlayLanguages() const { return overlayLangs_; }

    /**
     * \brief Display name (endonym) for a language code, for the picker.
     *
     * "en" returns the in-code English name ("English"); any other code
     * returns the `core.lang_name` read from its `lang_<code>.json`, falling
     * back to the code itself if unknown.
     *
     * \param code Language code (e.g. "en", "de").
     * \return CP437-encoded name; stable until the next rescan.
     */
    const char* languageName(const char* code) const;

    /**
     * \brief Callback invoked whenever the active translation table changes.
     *
     * Triggered on:
     *  - successful `loadOverlay()` (initial load or reload)
     *  - `setLanguageCode()` switching to a different language
     *
     * UI code uses this to refresh any cached label pointers - menu items hold
     * `const char*` into either rodata (English fallback) or the overlay's
     * PSRAM-backed string storage, both of which are invalidated by a language
     * change.
     */
    using LanguageChangedCallback = std::function<void()>;
    void setOnLanguageChanged(LanguageChangedCallback cb) { onChanged_ = std::move(cb); }

private:
    I18n();

    void registerCoreEnglishTable();
    bool sortIfNeeded() const;
    const char* enLookup(const char* key) const;
    const char* overlayLookup(const char* key) const;
    void loadLanguageFromNvs();
    void saveLanguageToNvs();

    /// Scan OVERLAY_DIR for `lang_<code>.json` files into `overlayLangs_`.
    void scanAvailableLanguages();
    /// Parse `lang_<currentLang_>.json` into `activeOverlay_`. \return false on error.
    bool loadActiveOverlayFile();

    mutable std::vector<I18nEntry> en_;
    mutable bool                   enSorted_ = false;

    // Active-language overlay stored entirely in PSRAM: a packed
    // "key\0value\0..." blob plus a key-sorted reference index for binary
    // search. Keeps the (potentially large) translation table off the scarce
    // internal heap, which WiFi/BLE need for contiguous allocations.
    struct OverlayRef { const char* key; const char* value; };
    cdc::core::PsramUniquePtr<char>       overlayBlob_;
    cdc::core::PsramUniquePtr<OverlayRef> overlayRefs_;
    std::size_t                           overlayCount_ = 0;
    std::vector<OverlayLanguage>          overlayLangs_;

    std::string currentLang_ = "en";

    LanguageChangedCallback onChanged_;
};

/// Look up a translation by string key.
inline const char* tr(const char* key)
{
    return I18n::instance().tr(key);
}

}  // namespace cdc::ui
