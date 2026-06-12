#pragma once

#include "cdc_core/IModule.h"
#include "cdc_core/TropicStorage.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace cdc::core {

/**
 * \brief Manages logical-to-physical RMEM slot mapping for module storage layers.
 *
 * Encapsulates slot range bookkeeping and free-slot discovery shared by
 * `OathStore`, `PasswordStore`, and other storage layers backed by the
 * Tropic secure element.
 */
class SlotManager {
public:
    /**
     * \brief Configures logical-to-physical slot mapping using raw values.
     * \param start First RMEM slot.
     * \param end Last RMEM slot.
     * \param moduleId Owning module identifier.
     */
    void setSlotRange(uint16_t start, uint16_t end, uint8_t moduleId) {
        if (start > end || start == 0 || end == 0) {
            hasSlotRange_ = false;
            rmemStart_ = 0;
            rmemEnd_ = 0;
            moduleId_ = 0;
            return;
        }
        hasSlotRange_ = true;
        rmemStart_ = start;
        rmemEnd_ = end;
        moduleId_ = moduleId;
    }

    /**
     * \brief Configures logical-to-physical slot mapping from a SlotRange struct.
     * \param range Slot range descriptor (only RMEM fields are consumed).
     */
    void setSlotRange(const IModule::SlotRange& range) {
        if (!range.hasRmem) {
            setSlotRange(0, 0, 0);
            return;
        }
        setSlotRange(range.rmemStart, range.rmemEnd, range.moduleId);
    }

    /**
     * \brief Returns available entry capacity from configured slot range.
     * \return Number of addressable logical entries, or `0` when no range is set.
     */
    uint16_t capacity() const {
        if (!hasSlotRange_) return 0;
        return static_cast<uint16_t>(rmemEnd_ - rmemStart_ + 1);
    }

    /**
     * \brief Converts logical index to physical RMEM slot.
     * \param logicalIndex Logical index.
     * \param slotOut Output physical slot.
     * \return `true` on valid mapping.
     */
    bool toPhysicalSlot(uint16_t logicalIndex, uint16_t* slotOut) const {
        if (!slotOut) return false;
        if (!hasSlotRange_) return false;
        uint32_t slot = static_cast<uint32_t>(rmemStart_) + logicalIndex;
        if (slot > rmemEnd_) return false;
        *slotOut = static_cast<uint16_t>(slot);
        return true;
    }

    /**
     * \brief Converts physical RMEM slot to logical index.
     * \param slot Physical slot.
     * \param logicalIndexOut Output logical index.
     * \return `true` on valid mapping.
     */
    bool toLogicalSlot(uint16_t slot, uint16_t* logicalIndexOut) const {
        if (!logicalIndexOut) return false;
        if (!hasSlotRange_) return false;
        if (slot < rmemStart_ || slot > rmemEnd_) return false;
        *logicalIndexOut = static_cast<uint16_t>(slot - rmemStart_);
        return true;
    }

    /**
     * \brief Finds first free physical slot in configured range.
     * \param slotOut Output physical slot.
     * \return `true` if a free slot was found.
     */
    bool findFreeSlot(uint16_t* slotOut) const {
        if (!slotOut) return false;
        if (!hasSlotRange_) return false;

        uint16_t cap = capacity();
        if (cap == 0) return false;
        auto used = std::unique_ptr<bool[]>(new (std::nothrow) bool[cap]);
        if (!used) return false;
        std::memset(used.get(), 0, cap * sizeof(bool));

        struct Ctx {
            bool* used;
            uint16_t base;
            uint16_t cap;
        } ctx = { used.get(), rmemStart_, cap };

        auto cb = [](uint16_t slot, const TropicStorage::CacheEntry&, void* user) {
            auto* c = static_cast<Ctx*>(user);
            if (slot < c->base) return;
            uint16_t idx = slot - c->base;
            if (idx < c->cap) {
                c->used[idx] = true;
            }
        };

        TropicStorage::instance().forEachSlot(
            moduleId_, rmemStart_, rmemEnd_, cb, &ctx);

        for (uint16_t i = 0; i < cap; i++) {
            if (!used[i]) {
                uint16_t candidate = static_cast<uint16_t>(rmemStart_ + i);
                if (candidate <= rmemEnd_) {
                    *slotOut = candidate;
                    return true;
                }
                return false;
            }
        }

        return false;
    }

    /**
     * \brief Returns whether a valid slot range is configured.
     * \return `true` when slot range was set successfully.
     */
    bool hasSlotRange() const { return hasSlotRange_; }

    /**
     * \brief Returns configured RMEM start slot.
     * \return First RMEM slot in the configured range.
     */
    uint16_t rmemStart() const { return rmemStart_; }

    /**
     * \brief Returns configured RMEM end slot.
     * \return Last RMEM slot in the configured range.
     */
    uint16_t rmemEnd() const { return rmemEnd_; }

    /**
     * \brief Returns configured owning module identifier.
     * \return Module identifier used for slot ownership filtering.
     */
    uint8_t moduleId() const { return moduleId_; }

protected:
    bool hasSlotRange_ = false;
    uint16_t rmemStart_ = 0;
    uint16_t rmemEnd_ = 0;
    uint8_t moduleId_ = 0;
};

} // namespace cdc::core
