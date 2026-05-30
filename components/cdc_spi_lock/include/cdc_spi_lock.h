#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace cdc::hal {

/**
 * \brief Recursive mutex serializing all access to the shared SPI bus
 *        (SPI2_HOST), shared by the e-paper display and the TROPIC01 secure
 *        element.
 *
 * The display and the secure element are two devices on one SPI bus. A TROPIC01
 * operation is many CS-framed transfers; if a display transfer slips between
 * them while the chip's CS is asserted, the chip sees a corrupt frame and
 * latches a tamper alarm that locks the whole device (recoverable only by a
 * physical power cycle). Both sides take this single lock - the secure element
 * around each complete operation, the display around each transfer - so the two
 * can never interleave on the bus.
 *
 * Lives in its own leaf component (depends only on freertos) so both cdc_hal
 * (secure element) and CalEPD (display) can use it without a dependency cycle.
 * Created once on first use (thread-safe); the first caller is the secure
 * element boot handshake on the single startup task, before any other SPI task
 * exists. Returns nullptr only on allocation failure (fatal at boot).
 */
SemaphoreHandle_t sharedSpiLock();

/**
 * \brief RAII recursive take/give of \ref sharedSpiLock for one scope.
 */
class SpiBusGuard {
public:
    SpiBusGuard() : lock_(sharedSpiLock()) {
        if (lock_) xSemaphoreTakeRecursive(lock_, portMAX_DELAY);
    }
    ~SpiBusGuard() {
        if (lock_) xSemaphoreGiveRecursive(lock_);
    }
    SpiBusGuard(const SpiBusGuard&) = delete;
    SpiBusGuard& operator=(const SpiBusGuard&) = delete;

private:
    SemaphoreHandle_t lock_;
};

}  // namespace cdc::hal
