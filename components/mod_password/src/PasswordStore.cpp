#include "mod_password/PasswordStore.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

static const char* TAG = "PASSWORD";

namespace cdc::mod_password {

#pragma pack(push, 1)
struct PasswordPayload {
    char title[PasswordStore::TITLE_LEN];
    char username[PasswordStore::USERNAME_LEN];
    char password[PasswordStore::PASSWORD_LEN];
    char url[PasswordStore::URL_LEN];
    uint8_t totpSlot;
    char notes[PasswordStore::NOTES_LEN];
};
#pragma pack(pop)

static_assert(sizeof(PasswordPayload) == PasswordStore::PAYLOAD_MAX, "Password payload size mismatch");

/**
 * \brief Copies text into bounded destination buffer.
 * \param dst Destination buffer.
 * \param dstSize Destination size.
 * \param src Source string.
 */
static void copyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

/**
 * \brief Returns singleton password store instance.
 * \return Store singleton reference.
 */
PasswordStore& PasswordStore::instance() {
    static PasswordStore inst;
    return inst;
}

/**
 * \brief Configures logical-to-physical slot mapping for password entries.
 * \param range Slot range descriptor (RMEM fields are consumed).
 */
void PasswordStore::setSlotRange(const cdc::core::IModule::SlotRange& range) {
    slots_.setSlotRange(range);
}

/**
 * \brief Reads one password entry from secure-element storage.
 * \param slot Logical slot index.
 * \param out Output entry.
 * \return `true` on success.
 */
bool PasswordStore::readEntry(uint16_t slot, PasswordEntry* out) const {
    if (!out) return false;
    if (!slots_.hasSlotRange()) return false;
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    cdc::hal::ISecureElement::RMemHeader header = {};
    PasswordPayload payload = {};
    uint16_t payloadLen = 0;

    auto res = se->rmemReadWithHeader(physSlot, &header,
                                      reinterpret_cast<uint8_t*>(&payload),
                                      sizeof(payload), &payloadLen);
    if (res != cdc::hal::SeResult::OK) {
        return false;
    }

    if (header.moduleId != slots_.moduleId()) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (payload.title[0]) {
        copyText(out->title, sizeof(out->title), payload.title);
    } else {
        copyText(out->title, sizeof(out->title), header.name);
    }
    copyText(out->username, sizeof(out->username), payload.username);
    copyText(out->password, sizeof(out->password), payload.password);
    copyText(out->url, sizeof(out->url), payload.url);
    out->totpSlot = payload.totpSlot;
    copyText(out->notes, sizeof(out->notes), payload.notes);

    return true;
}

/**
 * \brief Adds a new password entry into first free slot.
 * \param entry Entry data.
 * \return `true` on successful write.
 */
bool PasswordStore::addEntry(const PasswordEntry& entry) {
    if (!slots_.hasSlotRange()) return false;
    uint16_t slot = 0;
    if (!slots_.findFreeSlot(&slot)) {
        LOG_W(TAG, "No free password slots");
        return false;
    }

    PasswordPayload payload = {};
    copyText(payload.title, sizeof(payload.title), entry.title);
    copyText(payload.username, sizeof(payload.username), entry.username);
    copyText(payload.password, sizeof(payload.password), entry.password);
    copyText(payload.url, sizeof(payload.url), entry.url);
    payload.totpSlot = entry.totpSlot;
    copyText(payload.notes, sizeof(payload.notes), entry.notes);

    char headerName[cdc::hal::ISecureElement::RMEM_NAME_LEN + 1] = {};
    if (entry.title[0]) {
        copyText(headerName, sizeof(headerName), entry.title);
    } else {
        copyText(headerName, sizeof(headerName), "Password");
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    auto res = se->rmemWriteWithHeader(
        slot,
        slots_.moduleId(),
        headerName,
        0,
        reinterpret_cast<const uint8_t*>(&payload),
        sizeof(payload)
    );

    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to write slot %u (SeResult=%u, payload=%u bytes)",
              slot, static_cast<unsigned>(res),
              static_cast<unsigned>(sizeof(payload)));
        return false;
    }

    cdc::core::TropicStorage::instance().writeSlot(slots_.moduleId(), slot, headerName, 0);

    return true;
}

/**
 * \brief Updates existing password entry.
 * \param slot Logical slot index.
 * \param entry New entry data.
 * \return `true` on successful write.
 */
bool PasswordStore::updateEntry(uint16_t slot, const PasswordEntry& entry) {
    if (!slots_.hasSlotRange()) return false;
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;

    PasswordPayload payload = {};
    copyText(payload.title, sizeof(payload.title), entry.title);
    copyText(payload.username, sizeof(payload.username), entry.username);
    copyText(payload.password, sizeof(payload.password), entry.password);
    copyText(payload.url, sizeof(payload.url), entry.url);
    payload.totpSlot = entry.totpSlot;
    copyText(payload.notes, sizeof(payload.notes), entry.notes);

    char headerName[cdc::hal::ISecureElement::RMEM_NAME_LEN + 1] = {};
    if (entry.title[0]) {
        copyText(headerName, sizeof(headerName), entry.title);
    } else {
        copyText(headerName, sizeof(headerName), "Password");
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    auto res = se->rmemWriteWithHeader(
        physSlot,
        slots_.moduleId(),
        headerName,
        0,
        reinterpret_cast<const uint8_t*>(&payload),
        sizeof(payload)
    );

    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to write slot %u", slot);
        return false;
    }

    cdc::core::TropicStorage::instance().writeSlot(slots_.moduleId(), physSlot, headerName, 0);

    return true;
}

/**
 * \brief Deletes entry at logical slot index.
 * \param slot Logical slot index.
 * \return `true` on successful erase.
 */
bool PasswordStore::deleteEntry(uint16_t slot) {
    if (!slots_.hasSlotRange()) return false;
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    auto res = se->rmemErase(physSlot);
    if (res != cdc::hal::SeResult::OK) {
        return false;
    }

    cdc::core::TropicStorage::instance().eraseSlot(slots_.moduleId(), physSlot);
    return true;
}

/**
 * \brief Case-insensitive title comparison helper.
 * \param a First title.
 * \param b Second title.
 * \return Negative, zero, or positive compare result.
 */
int PasswordStore::compareTitles(const char* a, const char* b) {
    if (!a) return b ? -1 : 0;
    if (!b) return 1;
    while (*a || *b) {
        int ca = *a ? std::tolower(static_cast<unsigned char>(*a)) : 0;
        int cb = *b ? std::tolower(static_cast<unsigned char>(*b)) : 0;
        if (ca != cb) return ca - cb;
        if (*a) ++a;
        if (*b) ++b;
    }
    return 0;
}

/**
 * \brief Lists entries sorted alphabetically by title.
 * \param entries Output entry index array.
 * \param maxEntries Maximum writable entries.
 * \param countOut Output number of entries.
 * \return `true` on successful listing.
 */
bool PasswordStore::listEntriesSorted(EntryIndex* entries, uint16_t maxEntries, uint16_t* countOut) const {
    if (!entries || !countOut) return false;
    if (!slots_.hasSlotRange()) return false;

    struct Ctx {
        EntryIndex* entries;
        uint16_t* count;
        uint16_t max;
    } ctx = { entries, countOut, maxEntries };

    *countOut = 0;
    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        auto* ctx = static_cast<Ctx*>(user);
        if (!ctx || !ctx->entries || !ctx->count) return;
        if (*ctx->count >= ctx->max) return;

        uint16_t logical = 0;
        if (!PasswordStore::instance().toLogicalSlot(slot, &logical)) return;

        uint16_t idx = *ctx->count;
        copyText(ctx->entries[idx].title, sizeof(ctx->entries[idx].title), entry.name);
        ctx->entries[idx].slot = logical;
        (*ctx->count)++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        slots_.moduleId(), slots_.rmemStart(), slots_.rmemEnd(), cb, &ctx);

    std::sort(entries, entries + *countOut, [](const EntryIndex& a, const EntryIndex& b) {
        return PasswordStore::compareTitles(a.title, b.title) < 0;
    });

    return true;
}

/**
 * \brief Finds the logical slot of an entry with a matching title.
 *
 * Title is the entry's natural identity (it is the secure-element header name).
 * Comparison is case-insensitive to match the list ordering.
 *
 * \param title Title to look up.
 * \param logicalSlotOut Output logical slot index of the first match.
 * \return `true` if a matching entry exists.
 */
bool PasswordStore::findByTitle(const char* title, uint16_t* logicalSlotOut) const {
    if (!title || !logicalSlotOut) return false;
    if (!slots_.hasSlotRange()) return false;

    struct Ctx {
        const char* target;
        uint16_t slot;
        bool found;
    } ctx = { title, 0, false };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        auto* c = static_cast<Ctx*>(user);
        if (c->found) return;
        if (PasswordStore::compareTitles(entry.name, c->target) == 0) {
            uint16_t logical = 0;
            if (PasswordStore::instance().toLogicalSlot(slot, &logical)) {
                c->slot = logical;
                c->found = true;
            }
        }
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        slots_.moduleId(), slots_.rmemStart(), slots_.rmemEnd(), cb, &ctx);

    if (!ctx.found) return false;
    *logicalSlotOut = ctx.slot;
    return true;
}

/**
 * \brief Finds first free logical slot in this module's range.
 * \param logicalSlotOut Output logical slot index.
 * \return `true` if a free slot was found.
 */
bool PasswordStore::findFreeLogicalSlot(uint16_t* logicalSlotOut) const {
    if (!logicalSlotOut) return false;
    uint16_t physSlot = 0;
    if (!slots_.findFreeSlot(&physSlot)) return false;
    return toLogicalSlot(physSlot, logicalSlotOut);
}

} // namespace cdc::mod_password
