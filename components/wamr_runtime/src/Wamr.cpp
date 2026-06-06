#include "wamr_runtime/Wamr.h"

#include <mutex>

extern "C" {
#include "wasm_export.h"
void wamr_psram_alloc_fill(RuntimeInitArgs *args);
void wamr_install_enlarge_error_log(void);

// Log bridges implemented in wamr_log.cpp where cdc_log.h's `log_level_t`
// can be included without clashing with WAMR's identically-named typedef.
void wamr_log_info (const char *msg);
void wamr_log_error(const char *msg);
}

namespace cdc::wamr {

namespace {
std::once_flag s_init_flag;
bool           s_ready = false;
}  // namespace

bool init()
{
    std::call_once(s_init_flag, []() {
        static RuntimeInitArgs args;
        wamr_psram_alloc_fill(&args);
        if (!wasm_runtime_full_init(&args)) {
            wamr_log_error("wasm_runtime_full_init failed");
            return;
        }
        wamr_install_enlarge_error_log();
        wamr_log_info("WAMR ready (Fast Interpreter + AOT, PSRAM allocator)");
        s_ready = true;
    });
    return s_ready;
}

void deinit() noexcept
{
    if (!s_ready) return;
    wasm_runtime_destroy();
    s_ready = false;
}

bool isReady() noexcept
{
    return s_ready;
}

}  // namespace cdc::wamr
