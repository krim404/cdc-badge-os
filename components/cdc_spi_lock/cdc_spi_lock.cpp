#include "cdc_spi_lock.h"

namespace cdc::hal {

SemaphoreHandle_t sharedSpiLock() {
    // Thread-safe one-time init via a guarded function-local static (ESP-IDF
    // enables C++ guarded statics). xSemaphoreCreateRecursiveMutex runs under
    // the static guard's mutex, not a critical section, so the allocation is
    // safe. One instance is shared across all callers.
    static SemaphoreHandle_t lock = xSemaphoreCreateRecursiveMutex();
    return lock;
}

}  // namespace cdc::hal
