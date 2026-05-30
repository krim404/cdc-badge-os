/**
 * \file Raii.h
 * \brief Shared RAII wrappers for firmware resources.
 *
 * Single source of truth for resource ownership across the codebase. Each
 * type wraps a raw handle so destructors run on every exit path and manual
 * cleanup pairs disappear. Mirrors Rust's `Box<T>` + `Drop` idiom: ownership
 * is unique, copying is forbidden, moving transfers the handle.
 *
 * \par Provided wrappers
 *  - PsramUniquePtr<T>  - PSRAM array allocations via heap_caps
 *  - CStdUniquePtr<T>   - malloc/realloc buffers freed with std::free
 *  - FilePtr            - FILE* freed with std::fclose
 *  - NvsScope           - nvs_handle_t opened on construction, closed on destruction
 *  - MutexGuard         - FreeRTOS semaphore taken on construction, given on destruction
 */

#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace cdc::core {

/// Deleter for buffers allocated via heap_caps_malloc.
struct CapsFreeDeleter {
    void operator()(void* p) const noexcept
    {
        if (p) heap_caps_free(p);
    }
};

/// PSRAM-backed unique_ptr for byte/struct arrays.
/// Use psramAlloc<T>(n) to construct; default-constructed instance owns nothing.
template <typename T>
using PsramUniquePtr = std::unique_ptr<T[], CapsFreeDeleter>;

/**
 * \brief Allocate \p count elements of T in PSRAM (8-bit capable region).
 * \return Empty PsramUniquePtr if allocation failed.
 */
template <typename T>
PsramUniquePtr<T> psramAlloc(std::size_t count) noexcept
{
    auto* raw = static_cast<T*>(
        heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return PsramUniquePtr<T>{raw};
}

/// Deleter for buffers allocated via malloc/realloc.
struct CStdFreeDeleter {
    void operator()(void* p) const noexcept
    {
        if (p) std::free(p);
    }
};

/// unique_ptr for buffers allocated via realloc/malloc (e.g. esp_http_client body chunks).
template <typename T>
using CStdUniquePtr = std::unique_ptr<T, CStdFreeDeleter>;

/// Deleter for FILE* opened with std::fopen.
struct FileCloseDeleter {
    void operator()(std::FILE* fp) const noexcept
    {
        if (fp) std::fclose(fp);
    }
};

/// unique_ptr for FILE* handles. Destructor calls std::fclose.
using FilePtr = std::unique_ptr<std::FILE, FileCloseDeleter>;

/**
 * \brief Open a FILE* and wrap it in a FilePtr.
 * \param path File path.
 * \param mode fopen mode string.
 * \return Empty FilePtr if open failed.
 */
inline FilePtr openFile(const char* path, const char* mode) noexcept
{
    return FilePtr{std::fopen(path, mode)};
}

/**
 * \brief RAII wrapper for an NVS handle.
 *
 * Opens the given namespace in the constructor and closes it in the destructor.
 * Non-copyable. Use bool conversion to check whether the open succeeded.
 *
 * \par Example
 * \code
 * NvsScope nvs("myns", NVS_READWRITE);
 * if (!nvs) return false;
 * nvs_set_str(nvs, "key", "value");
 * nvs.commit();
 * \endcode
 */
class NvsScope {
public:
    /// Construct an empty, closed scope. Status reports ESP_FAIL until opened.
    NvsScope() noexcept = default;

    /// Open \p ns in \p mode. Errors are silent; use bool conversion or status() to check.
    NvsScope(const char* ns, nvs_open_mode_t mode) noexcept
    {
        status_ = nvs_open(ns, mode, &handle_);
        if (status_ != ESP_OK) handle_ = 0;
    }

    ~NvsScope() noexcept
    {
        if (handle_) nvs_close(handle_);
    }

    NvsScope(const NvsScope&) = delete;
    NvsScope& operator=(const NvsScope&) = delete;

    NvsScope(NvsScope&& other) noexcept : handle_(other.handle_), status_(other.status_)
    {
        other.handle_ = 0;
        other.status_ = ESP_FAIL;
    }
    NvsScope& operator=(NvsScope&& other) noexcept
    {
        if (this != &other) {
            if (handle_) nvs_close(handle_);
            handle_ = other.handle_;
            status_ = other.status_;
            other.handle_ = 0;
            other.status_ = ESP_FAIL;
        }
        return *this;
    }

    /// True if the namespace was opened successfully.
    explicit operator bool() const noexcept { return status_ == ESP_OK; }

    /// Raw handle for the underlying nvs_* functions.
    operator nvs_handle_t() const noexcept { return handle_; }

    /// esp_err_t result of the original nvs_open call.
    esp_err_t status() const noexcept { return status_; }

    /// Commit pending writes. Caller checks the return value if needed.
    esp_err_t commit() noexcept
    {
        return handle_ ? nvs_commit(handle_) : ESP_FAIL;
    }

    /// Manually close before destruction. Idempotent.
    void close() noexcept
    {
        if (handle_) {
            nvs_close(handle_);
            handle_ = 0;
        }
        status_ = ESP_FAIL;
    }

private:
    nvs_handle_t handle_ = 0;
    esp_err_t    status_ = ESP_FAIL;
};

/**
 * \brief RAII wrapper for a FreeRTOS semaphore / mutex.
 *
 * Default constructor takes the semaphore with portMAX_DELAY; destructor gives
 * it back. Use the static tryLock factory for non-blocking acquisition. Non-copyable.
 *
 * Not safe to call from ISR context (use xSemaphoreTakeFromISR directly there).
 */
class MutexGuard {
public:
    /// Take \p sem blocking until acquired.
    explicit MutexGuard(SemaphoreHandle_t sem) noexcept : sem_(sem)
    {
        if (sem_) xSemaphoreTake(sem_, portMAX_DELAY);
    }

    ~MutexGuard() noexcept
    {
        if (sem_ && locked_) xSemaphoreGive(sem_);
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    MutexGuard(MutexGuard&& other) noexcept
        : sem_(other.sem_), locked_(other.locked_)
    {
        other.sem_ = nullptr;
        other.locked_ = false;
    }

    /// True if the semaphore is currently held.
    bool locked() const noexcept { return locked_; }

    /// Release the semaphore before destruction. Idempotent.
    void release() noexcept
    {
        if (sem_ && locked_) {
            xSemaphoreGive(sem_);
            locked_ = false;
        }
    }

    /// Try to acquire \p sem for up to \p ticks ticks. The returned guard is
    /// "locked" only if acquisition succeeded; check via .locked().
    static MutexGuard tryLock(SemaphoreHandle_t sem, TickType_t ticks) noexcept
    {
        MutexGuard g{};
        g.sem_ = sem;
        if (sem && xSemaphoreTake(sem, ticks) == pdTRUE) {
            g.locked_ = true;
        }
        return g;
    }

private:
    MutexGuard() noexcept : sem_(nullptr), locked_(false) {}

    SemaphoreHandle_t sem_    = nullptr;
    bool              locked_ = true;
};

/**
 * \brief Scoped guard for a FreeRTOS recursive mutex.
 *
 * Takes the recursive semaphore on construction and gives it back on
 * destruction. Tolerates a `nullptr` handle (no-op), so it can guard an
 * optional/opt-in lock. The same task may nest acquisitions, which a plain
 * MutexGuard would self-deadlock on.
 *
 * Not safe to call from ISR context.
 */
class RecursiveMutexGuard {
public:
    /// Take \p sem blocking until acquired; no-op when \p sem is nullptr.
    explicit RecursiveMutexGuard(SemaphoreHandle_t sem) noexcept : sem_(sem)
    {
        if (sem_) xSemaphoreTakeRecursive(sem_, portMAX_DELAY);
    }

    ~RecursiveMutexGuard() noexcept
    {
        if (sem_) xSemaphoreGiveRecursive(sem_);
    }

    RecursiveMutexGuard(const RecursiveMutexGuard&) = delete;
    RecursiveMutexGuard& operator=(const RecursiveMutexGuard&) = delete;

private:
    SemaphoreHandle_t sem_ = nullptr;
};

}  // namespace cdc::core
