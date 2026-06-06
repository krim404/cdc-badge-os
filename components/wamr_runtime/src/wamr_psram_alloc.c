/**
 * \file wamr_psram_alloc.c
 * \brief PSRAM-resident allocator hooks for WAMR.
 *
 * All WAMR allocations (runtime structures, module bytecode, linear memory,
 * operand stack) flow through these wrappers, which forward to
 * heap_caps_*(MALLOC_CAP_SPIRAM). The badge's internal DRAM stays untouched
 * by the runtime itself.
 */

#include "esp_heap_caps.h"
#include "wasm_export.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern void wamr_log_error(const char *msg);

static void *wamr_psram_malloc(unsigned int size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *wamr_psram_realloc(void *ptr, unsigned int size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void wamr_psram_free(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

void wamr_psram_alloc_fill(RuntimeInitArgs *args)
{
    memset(args, 0, sizeof(*args));
    args->mem_alloc_type = Alloc_With_Allocator;
    args->mem_alloc_option.allocator.malloc_func  = (void *)wamr_psram_malloc;
    args->mem_alloc_option.allocator.realloc_func = (void *)wamr_psram_realloc;
    args->mem_alloc_option.allocator.free_func    = (void *)wamr_psram_free;
}

// A failed memory.grow inside a Rust guest aborts via the unreachable opcode
// without routing through the guest panic hook, so the trap is otherwise a bare
// "unreachable". This callback surfaces the reason and the PSRAM state at the
// point of failure.
static void wamr_enlarge_mem_error_log(uint32_t inc_page_count,
                                       uint64_t current_memory_size,
                                       uint32_t memory_index,
                                       enlarge_memory_error_reason_t failure_reason,
                                       wasm_module_inst_t instance,
                                       wasm_exec_env_t exec_env,
                                       void *user_data)
{
    (void)memory_index;
    (void)instance;
    (void)exec_env;
    (void)user_data;

    size_t want_kb = ((size_t)inc_page_count * 65536u) / 1024u;
    size_t free_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u;
    size_t largest_kb = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u;

    char buf[208];
    snprintf(buf, sizeof(buf),
             "linear memory grow failed: reason=%s want=+%uKB cur=%uKB "
             "PSRAM free=%uKB largest_block=%uKB",
             failure_reason == MAX_SIZE_REACHED ? "MAX_SIZE_REACHED" : "ALLOC_FAIL",
             (unsigned)want_kb,
             (unsigned)((size_t)(current_memory_size / 1024u)),
             (unsigned)free_kb,
             (unsigned)largest_kb);
    wamr_log_error(buf);
}

void wamr_install_enlarge_error_log(void)
{
    wasm_runtime_set_enlarge_mem_error_callback(wamr_enlarge_mem_error_log, NULL);
}
