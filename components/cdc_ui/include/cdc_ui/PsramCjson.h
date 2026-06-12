/**
 * \file PsramCjson.h
 * \brief RAII scope that routes cJSON allocations to PSRAM.
 *
 * Keeps cJSON parse/print trees off the scarce internal heap by swapping the
 * cJSON allocator hooks to PSRAM for the scope's lifetime and restoring the
 * defaults on destruction.
 */

#pragma once

#include "cJSON.h"
#include "esp_heap_caps.h"

#include <cstddef>
#include <cstdlib>

namespace cdc::ui {

/**
 * \brief Routes cJSON allocations to PSRAM for the lifetime of the scope.
 *
 * cJSON hooks are GLOBAL process state, so this is safe only for
 * single-threaded use: do not nest scopes across threads or run two cJSON
 * trees concurrently under different hook sets.
 */
struct PsramCjsonScope {
    static void* alloc(std::size_t sz) {
        return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    PsramCjsonScope() {
        cJSON_Hooks hooks{alloc, std::free};
        cJSON_InitHooks(&hooks);
    }
    ~PsramCjsonScope() { cJSON_InitHooks(nullptr); }
};

} // namespace cdc::ui
