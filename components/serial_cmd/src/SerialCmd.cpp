/**
 * Serial Command Processor Implementation
 * Handles input buffering, line editing, and command dispatch
 */

#include "serial_cmd/SerialCmd.h"
#include "serial_cmd/Console.h"
#include "serial_cmd/ICommandRegistry.h"
#include "cdc_core/feature_flags.h"
#include "cdc_core/PinManager.h"
#include "cdc_core/TropicSlotMap.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/FactoryReset.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"
#include "cdc_views/RenderHelpers.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "SERIAL";

// Forward declaration for WiFi serial command registration
namespace cdc::serial {
void registerWifiCommands();
}

namespace cdc::serial {

/**
 * \brief Internal constants used by serial command processing.
 */

static constexpr size_t HISTORY_MAX = 10;
static constexpr size_t HEX_DUMP_WIDTH = 16;
static constexpr size_t NVS_KEY_MAX_LEN = 15;
static constexpr size_t NVS_NAMESPACE_MAX_LEN = 15;
static constexpr int YEAR_MIN = 2020;
static constexpr int YEAR_MAX = 2100;
static constexpr uint32_t WIPE_PROGRESS_INTERVAL = 64;

/**
 * \brief Global static state for line editing and command dispatch.
 */

static char s_cmdBuffer[SerialCmd::CMD_BUFFER_SIZE] = {};
static size_t s_cmdBufferPos = 0;
static bool s_initialized = false;

/**
 * \brief Command history ring buffer allocated in PSRAM.
 */
EXT_RAM_BSS_ATTR static char s_historyBuffer[HISTORY_MAX][SerialCmd::CMD_BUFFER_SIZE];
static size_t s_historyCount = 0;
static size_t s_historyHead = 0;
static size_t s_historyPos = 0;

/**
 * \brief Escape-sequence parser state for ANSI key handling.
 */
enum class EscState : uint8_t { NONE, ESC, BRACKET };
static EscState s_escState = EscState::NONE;

/**
 * \brief Optional callbacks injected by higher-level modules.
 */
static TextChangeCallback s_textCallback = nullptr;
static TimeChangeCallback s_timeCallback = nullptr;

/**
 * \brief Session authentication flags and timeout baseline.
 */
static bool s_authenticated = false;
static uint64_t s_authTimestamp = 0;

/**
 * \brief General-purpose helper functions.
 */

#if FEATURE_SECURE_SERIAL
/**
 * \brief Resets the command authentication timeout timer.
 * \return void
 */
static void resetAuthTimer() {
    if (s_authenticated) {
        s_authTimestamp = esp_timer_get_time();
    }
}
#endif

/**
 * \brief Adds a command line to the history ring buffer.
 * \param cmd Command string to store.
 * \return void
 */
static void historyAdd(const char* cmd) {
    if (!cmd || !*cmd) return;

    strncpy(s_historyBuffer[s_historyHead], cmd, SerialCmd::CMD_BUFFER_SIZE - 1);
    s_historyBuffer[s_historyHead][SerialCmd::CMD_BUFFER_SIZE - 1] = '\0';

    s_historyHead = (s_historyHead + 1) % HISTORY_MAX;
    if (s_historyCount < HISTORY_MAX) {
        s_historyCount++;
    }
}

/**
 * \brief Returns a history entry by reverse index (`0` = newest).
 * \param idx Reverse history index.
 * \return Pointer to the command string, or `nullptr` if out of range.
 */
static const char* historyGet(size_t idx) {
    if (idx >= s_historyCount) return nullptr;
    size_t pos = (s_historyHead + HISTORY_MAX - 1 - idx) % HISTORY_MAX;
    return s_historyBuffer[pos];
}

/**
 * \brief Clears the current console line and redraws it with new content.
 * \param newContent Replacement line content.
 * \param bufferPos In/out cursor position updated to new content length.
 * \return void
 */
static void redrawLine(const char* newContent, size_t& bufferPos) {
    while (bufferPos > 0) {
        Console::print("\b \b");
        bufferPos--;
    }

    if (newContent) {
        strncpy(s_cmdBuffer, newContent, SerialCmd::CMD_BUFFER_SIZE - 1);
        s_cmdBuffer[SerialCmd::CMD_BUFFER_SIZE - 1] = '\0';
        size_t len = strlen(s_cmdBuffer);
        bufferPos = len;
        Console::print(s_cmdBuffer);
    }
}

/**
 * \brief Slot parsing helpers for secure-element commands.
 */

/**
 * Result of slot parsing operation
 */
struct SlotParseResult {
    bool valid;
    long value;
};

/**
 * \brief Parses a slot number from a string argument.
 * \param args Input string containing the slot number.
 * \param maxSlot Maximum valid slot value (exclusive).
 * \param slotTypeName Name for error messages (for example, "ECC slot" or "R-Memory slot").
 * \return Parse result with validity flag and parsed value.
 */
static SlotParseResult parseSlotArg(const char* args, uint16_t maxSlot, const char* slotTypeName) {
    SlotParseResult result = {false, 0};

    if (!args || !*args) {
        Console::printf("Usage: Provide a %s number\r\n", slotTypeName);
        return result;
    }

    char* endptr = nullptr;
    long slotVal = strtol(args, &endptr, 10);

    if (endptr == args || *endptr != '\0' || slotVal < 0) {
        Console::printf("ERROR: Invalid %s number\r\n", slotTypeName);
        return result;
    }

    if (slotVal >= maxSlot) {
        Console::printf("ERROR: Invalid %s (0-%d)\r\n", slotTypeName, maxSlot - 1);
        return result;
    }

    result.valid = true;
    result.value = slotVal;
    return result;
}

/**
 * \brief Secure-element access helpers.
 */

/**
 * \brief Returns the secure element instance after availability validation.
 * \return Pointer to the secure element instance, or `nullptr` if unavailable.
 */
static hal::ISecureElement* getSecureElementWithCheck() {
    auto* se = hal::getSecureElementInstance();
    if (!se) {
        Console::printf("ERROR: Secure Element not available\r\n");
    }
    return se;
}

/**
 * \brief NVS utility helpers used by command handlers.
 */

/**
 * \brief Prints a bounded hex dump of binary data.
 * \param data Input binary buffer.
 * \param len Total data length.
 * \param maxBytes Maximum number of bytes to print.
 * \return void
 */
static void printHexDump(const uint8_t* data, size_t len, size_t maxBytes) {
    for (size_t i = 0; i < len && i < maxBytes; i += HEX_DUMP_WIDTH) {
        Console::printf("  %04X: ", (unsigned)i);
        for (size_t j = 0; j < HEX_DUMP_WIDTH && (i + j) < len; j++) {
            Console::printf("%02X ", data[i + j]);
        }
        Console::printf("\r\n");
    }
    if (len > maxBytes) {
        Console::printf("  ... (%d more bytes)\r\n", (int)(len - maxBytes));
    }
}

/**
 * \brief Returns a human-readable name for an NVS type value.
 * \param type NVS value type.
 * \return Static type-name string.
 */
static const char* getNvsTypeName(nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8:   return "u8";
        case NVS_TYPE_I8:   return "i8";
        case NVS_TYPE_U16:  return "u16";
        case NVS_TYPE_I16:  return "i16";
        case NVS_TYPE_U32:  return "u32";
        case NVS_TYPE_I32:  return "i32";
        case NVS_TYPE_U64:  return "u64";
        case NVS_TYPE_I64:  return "i64";
        case NVS_TYPE_STR:  return "str";
        case NVS_TYPE_BLOB: return "blob";
        default:            return "?";
    }
}

/**
 * \brief Finds the stored NVS type of a key by namespace iteration.
 * \param ns NVS namespace name.
 * \param key Key name to inspect.
 * \return Resolved NVS type, or `NVS_TYPE_ANY` if unknown.
 */
static nvs_type_t findNvsKeyType(const char* ns, const char* key) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    nvs_type_t keyType = NVS_TYPE_ANY;

    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strcmp(info.key, key) == 0) {
            keyType = info.type;
            break;
        }
        err = nvs_entry_next(&it);
        if (err != ESP_OK) break;
    }
    nvs_release_iterator(it);
    return keyType;
}

/**
 * \brief Prints an NVS value according to its stored type.
 * \param nvs Open NVS handle.
 * \param key Key name.
 * \param type Value type of the key.
 * \return void
 */
static void printNvsValue(nvs_handle_t nvs, const char* key, nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8: {
            uint8_t val;
            if (nvs_get_u8(nvs, key, &val) == ESP_OK) {
                Console::printf("%u (0x%02X)\r\n", val, val);
            }
            break;
        }
        case NVS_TYPE_I8: {
            int8_t val;
            if (nvs_get_i8(nvs, key, &val) == ESP_OK) {
                Console::printf("%d\r\n", val);
            }
            break;
        }
        case NVS_TYPE_U16: {
            uint16_t val;
            if (nvs_get_u16(nvs, key, &val) == ESP_OK) {
                Console::printf("%u (0x%04X)\r\n", val, val);
            }
            break;
        }
        case NVS_TYPE_I16: {
            int16_t val;
            if (nvs_get_i16(nvs, key, &val) == ESP_OK) {
                Console::printf("%d\r\n", val);
            }
            break;
        }
        case NVS_TYPE_U32: {
            uint32_t val;
            if (nvs_get_u32(nvs, key, &val) == ESP_OK) {
                Console::printf("%lu (0x%08lX)\r\n", (unsigned long)val, (unsigned long)val);
            }
            break;
        }
        case NVS_TYPE_I32: {
            int32_t val;
            if (nvs_get_i32(nvs, key, &val) == ESP_OK) {
                Console::printf("%ld\r\n", (long)val);
            }
            break;
        }
        case NVS_TYPE_U64: {
            uint64_t val;
            if (nvs_get_u64(nvs, key, &val) == ESP_OK) {
                Console::printf("%llu\r\n", (unsigned long long)val);
            }
            break;
        }
        case NVS_TYPE_I64: {
            int64_t val;
            if (nvs_get_i64(nvs, key, &val) == ESP_OK) {
                Console::printf("%lld\r\n", (long long)val);
            }
            break;
        }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(nvs, key, nullptr, &len) == ESP_OK && len > 0) {
                char* buf = static_cast<char*>(malloc(len));
                if (!buf) {
                    LOG_E(TAG, "Failed to allocate %d bytes for NVS string", (int)len);
                    Console::printf("(allocation failed)\r\n");
                    break;
                }
                if (nvs_get_str(nvs, key, buf, &len) == ESP_OK) {
                    Console::printf("\"%s\"\r\n", buf);
                }
                free(buf);
            }
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            if (nvs_get_blob(nvs, key, nullptr, &len) == ESP_OK && len > 0) {
                Console::printf("(blob, %d bytes)\r\n", (int)len);
                uint8_t* buf = static_cast<uint8_t*>(malloc(len));
                if (!buf) {
                    LOG_E(TAG, "Failed to allocate %d bytes for NVS blob", (int)len);
                    Console::printf("  (allocation failed)\r\n");
                    break;
                }
                if (nvs_get_blob(nvs, key, buf, &len) == ESP_OK) {
                    printHexDump(buf, len, len);
                }
                free(buf);
            }
            break;
        }
        default:
            Console::printf("(unknown type)\r\n");
            break;
    }
}

/**
 * \brief Date/time parsing and validation helpers.
 */

/**
 * \brief Retrieves the current time as `timeval` and local `tm`.
 * \param tv Output `timeval`.
 * \param tm Caller-owned output `tm` populated via `localtime_r`.
 * \return `true` if time conversion succeeded, otherwise `false`.
 */
static bool getCurrentTime(struct timeval& tv, struct tm& tm) {
    gettimeofday(&tv, nullptr);
    return localtime_r(&tv.tv_sec, &tm) != nullptr;
}

/**
 * \brief Sets system time from a populated local `tm` structure.
 * \param tm Input date/time structure.
 * \return `true` on success, otherwise `false`.
 */
static bool setSystemTime(struct tm* tm) {
    struct timeval tv;
    tv.tv_sec = mktime(tm);
    tv.tv_usec = 0;
    return settimeofday(&tv, nullptr) == 0;
}

/**
 * \brief System command handlers.
 */

/**
 * \brief Prints the registered command overview.
 * \param args Unused command arguments.
 */
static void cmdHelp(const char* args) {
    (void)args;
    getCommandRegistry().showHelp();
}

/**
 * \brief Replies with a liveness check response.
 * \param args Unused command arguments.
 */
static void cmdPing(const char* args) {
    (void)args;
    Console::printf("PONG\r\n");
}

/**
 * \brief Prints runtime status information for the device.
 * \param args Unused command arguments.
 */
static void cmdStatus(const char* args) {
    (void)args;
    Console::printf("=== System Status ===\r\n");
    Console::printf("Free heap: %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
    Console::printf("Min free heap: %lu bytes\r\n", (unsigned long)esp_get_minimum_free_heap_size());
    Console::printf("Uptime: %llu ms\r\n", esp_timer_get_time() / 1000ULL);
    Console::flush();
}

/**
 * \brief Prints heap and PSRAM usage statistics.
 * \param args Unused command arguments.
 */
/**
 * \brief Prints detailed per-task stack watermarks plus heap fragmentation.
 *        Heavier than cmdMem; only relevant for diagnostics.
 */
static void cmdMemInfo(const char* args) {
    (void)args;
    Console::printf("=== Detailed Memory Info ===\r\n");

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    Console::printf("\r\n-- Internal DRAM --\r\n");
    Console::printf("  total free      : %lu\r\n", (unsigned long)info.total_free_bytes);
    Console::printf("  total allocated : %lu\r\n", (unsigned long)info.total_allocated_bytes);
    Console::printf("  largest free    : %lu\r\n", (unsigned long)info.largest_free_block);
    Console::printf("  min ever free   : %lu\r\n", (unsigned long)info.minimum_free_bytes);
    Console::printf("  free blocks     : %lu\r\n", (unsigned long)info.free_blocks);
    Console::printf("  alloc blocks    : %lu\r\n", (unsigned long)info.allocated_blocks);

    heap_caps_get_info(&info, MALLOC_CAP_DMA);
    Console::printf("\r\n-- DMA-capable --\r\n");
    Console::printf("  total free      : %lu\r\n", (unsigned long)info.total_free_bytes);
    Console::printf("  largest free    : %lu\r\n", (unsigned long)info.largest_free_block);

    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    if (info.total_free_bytes + info.total_allocated_bytes > 0) {
        Console::printf("\r\n-- PSRAM --\r\n");
        Console::printf("  total free      : %lu\r\n", (unsigned long)info.total_free_bytes);
        Console::printf("  total allocated : %lu\r\n", (unsigned long)info.total_allocated_bytes);
        Console::printf("  largest free    : %lu\r\n", (unsigned long)info.largest_free_block);
    }

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t numTasks = uxTaskGetNumberOfTasks();
    TaskStatus_t* tasks = (TaskStatus_t*)calloc(numTasks, sizeof(TaskStatus_t));
    if (tasks) {
        numTasks = uxTaskGetSystemState(tasks, numTasks, nullptr);
        Console::printf("\r\n-- Tasks (%u) --\r\n", (unsigned)numTasks);
        Console::printf("  %-16s  Prio  StkMinFree  Stk@   State  Core\r\n", "Name");
        uint32_t totalStackFree = 0;
        for (UBaseType_t i = 0; i < numTasks; i++) {
            const char* st = "?";
            switch (tasks[i].eCurrentState) {
                case eRunning:   st = "RUN"; break;
                case eReady:     st = "RDY"; break;
                case eBlocked:   st = "BLK"; break;
                case eSuspended: st = "SUS"; break;
                case eDeleted:   st = "DEL"; break;
                case eInvalid:   st = "INV"; break;
            }
            BaseType_t coreId = -1;
#if INCLUDE_xTaskGetCoreID
            coreId = xTaskGetCoreID(tasks[i].xHandle);
#endif
            const char* stackLoc = "DRAM";
            if (tasks[i].pxStackBase != nullptr &&
                esp_ptr_external_ram(tasks[i].pxStackBase)) {
                stackLoc = "PSRAM";
            }
            Console::printf("  %-16s  %-4u  %-10lu  %-5s  %-5s  %d\r\n",
                            tasks[i].pcTaskName,
                            (unsigned)tasks[i].uxCurrentPriority,
                            (unsigned long)tasks[i].usStackHighWaterMark,
                            stackLoc,
                            st,
                            (int)coreId);
            totalStackFree += tasks[i].usStackHighWaterMark;
        }
        Console::printf("  (Sum stack headroom across all tasks: %lu B)\r\n",
                        (unsigned long)totalStackFree);
        free(tasks);
    }
#else
    Console::printf("\r\nTask list unavailable (FREERTOS_USE_TRACE_FACILITY=n)\r\n");
#endif

    Console::flush();
}

static void cmdMem(const char* args) {
    (void)args;
    Console::printf("=== Memory Usage ===\r\n");
    Console::printf("Heap (total): %lu / %lu bytes free\r\n",
                   (unsigned long)esp_get_free_heap_size(),
                   (unsigned long)heap_caps_get_total_size(MALLOC_CAP_DEFAULT));

    size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t intLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    Console::printf("Internal DRAM: %lu / %lu free (largest block %lu)\r\n",
                    (unsigned long)intFree,
                    (unsigned long)intTotal,
                    (unsigned long)intLargest);

    size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    Console::printf("DMA-capable: %lu free (largest %lu)\r\n",
                    (unsigned long)dmaFree,
                    (unsigned long)dmaLargest);

    size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psramTotal > 0) {
        Console::printf("PSRAM: %lu / %lu bytes free\r\n",
                       (unsigned long)psramFree,
                       (unsigned long)psramTotal);
    }
    Console::flush();
}

/**
 * \brief Reboots the device after flushing serial output.
 * \param args Unused command arguments.
 */
static void cmdReboot(const char* args) {
    (void)args;
    Console::printf("Rebooting...\r\n");
    Console::flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/**
 * \brief Displays the error log or clears it when `CLEAR` is passed.
 * \param args Optional action argument.
 */
static void cmdErrorLog(const char* args) {
    if (args && strcmp(args, "CLEAR") == 0) {
        error_log_clear();
        Console::printf("Error log cleared.\r\n");
    } else {
        error_log_dump();
    }
}

/**
 * \brief NVS command handlers.
 */

/**
 * \brief Erases all NVS data after explicit confirmation.
 * \param args Confirmation argument (`YES` required).
 */
static void cmdNvsClear(const char* args) {
    if (!args || strcmp(args, "YES") != 0) {
        Console::printf("WARNING: This will ERASE ALL NVS data!\r\n");
        Console::printf("  - All module settings\r\n");
        Console::printf("  - All stored preferences\r\n");
        Console::printf("  - WiFi credentials\r\n");
        Console::printf("  - Timezone settings\r\n");
        Console::printf("\r\nTo proceed, type: NVS_CLEAR YES\r\n");
        return;
    }

    Console::printf("Clearing NVS...\r\n");
    esp_err_t err = core::wipeNvs();
    if (err != ESP_OK) {
        Console::printf("ERROR: NVS wipe failed (%s)\r\n", esp_err_to_name(err));
        return;
    }
    Console::printf("OK: NVS cleared. Reboot recommended.\r\n");
}

/**
 * \brief Lists NVS entries, optionally filtered by namespace.
 * \param args Optional namespace filter.
 */
static void cmdNvsList(const char* args) {
    const char* nsFilter = (args && *args) ? args : nullptr;

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", nsFilter, NVS_TYPE_ANY, &it);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (nsFilter) {
            Console::printf("Namespace '%s' not found or empty\r\n", nsFilter);
        } else {
            Console::printf("NVS is empty\r\n");
        }
        return;
    }

    if (err != ESP_OK) {
        Console::printf("ERROR: nvs_entry_find failed (%s)\r\n", esp_err_to_name(err));
        return;
    }

    Console::printf("=== NVS Contents ===\r\n");
    if (nsFilter) {
        Console::printf("Namespace: %s\r\n", nsFilter);
    }

    char lastNs[NVS_NAMESPACE_MAX_LEN + 1] = {0};
    int count = 0;

    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (!nsFilter && strcmp(lastNs, info.namespace_name) != 0) {
            strncpy(lastNs, info.namespace_name, sizeof(lastNs) - 1);
            Console::printf("\r\n[%s]\r\n", info.namespace_name);
        }

        Console::printf("  %s (%s)\r\n", info.key, getNvsTypeName(info.type));
        count++;

        err = nvs_entry_next(&it);
        if (err != ESP_OK) break;
    }

    nvs_release_iterator(it);
    Console::printf("\r\nTotal: %d entries\r\n", count);
}

/**
 * \brief Reads and prints a single NVS key value.
 * \param args Arguments in the form `<namespace> <key>`.
 */
static void cmdNvsRead(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: NVS_READ <namespace> <key>\r\n");
        return;
    }

    char ns[NVS_NAMESPACE_MAX_LEN + 1] = {0};
    char key[NVS_KEY_MAX_LEN + 1] = {0};
    if (sscanf(args, "%15s %15s", ns, key) != 2) {
        Console::printf("Usage: NVS_READ <namespace> <key>\r\n");
        return;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        Console::printf("ERROR: Cannot open namespace '%s' (%s)\r\n", ns, esp_err_to_name(err));
        return;
    }

    nvs_type_t keyType = findNvsKeyType(ns, key);
    if (keyType == NVS_TYPE_ANY) {
        Console::printf("ERROR: Key '%s' not found in namespace '%s'\r\n", key, ns);
        nvs_close(nvs);
        return;
    }

    Console::printf("%s.%s = ", ns, key);
    printNvsValue(nvs, key, keyType);
    nvs_close(nvs);
}

/**
 * \brief Deletes an NVS key or an entire namespace.
 * \param args Arguments in the form `<namespace> [key]`.
 */
static void cmdNvsDel(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: NVS_DEL <namespace> [key]\r\n");
        Console::printf("  Without key: erases entire namespace\r\n");
        return;
    }

    char ns[NVS_NAMESPACE_MAX_LEN + 1] = {0};
    char key[NVS_KEY_MAX_LEN + 1] = {0};
    int parsed = sscanf(args, "%15s %15s", ns, key);

    if (parsed < 1) {
        Console::printf("Usage: NVS_DEL <namespace> [key]\r\n");
        return;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        Console::printf("ERROR: Cannot open namespace '%s' (%s)\r\n", ns, esp_err_to_name(err));
        return;
    }

    if (parsed == 1 || key[0] == '\0') {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            nvs_commit(nvs);
            Console::printf("OK: Namespace '%s' erased\r\n", ns);
        } else {
            Console::printf("ERROR: Erase failed (%s)\r\n", esp_err_to_name(err));
        }
    } else {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_OK) {
            nvs_commit(nvs);
            Console::printf("OK: Key '%s.%s' deleted\r\n", ns, key);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            Console::printf("ERROR: Key '%s' not found\r\n", key);
        } else {
            Console::printf("ERROR: Delete failed (%s)\r\n", esp_err_to_name(err));
        }
    }

    nvs_close(nvs);
}

/**
 * \brief Date/time command handlers.
 */

/**
 * \brief Prints the current local time.
 * \param args Unused command arguments.
 */
static void cmdGetTime(const char* args) {
    (void)args;
    struct timeval tv;
    struct tm tm;
    if (getCurrentTime(tv, tm)) {
        Console::printf("%02d:%02d:%02d\r\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        Console::printf("--:--:--\r\n");
    }
}

/**
 * \brief Prints the current local date.
 * \param args Unused command arguments.
 */
static void cmdGetDate(const char* args) {
    (void)args;
    struct timeval tv;
    struct tm tm;
    if (getCurrentTime(tv, tm)) {
        Console::printf("%02d.%02d.%04d\r\n", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    } else {
        Console::printf("--.---.----\r\n");
    }
}

/**
 * \brief Updates the system clock time component.
 * \param args Time string in format `HH:MM:SS`.
 */
static void cmdSetTime(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: SET_TIME HH:MM:SS\r\n");
        return;
    }
    int h, m, s;
    if (sscanf(args, "%d:%d:%d", &h, &m, &s) != 3) {
        Console::printf("ERROR: Invalid format. Use HH:MM:SS\r\n");
        return;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
        Console::printf("ERROR: Invalid time values\r\n");
        return;
    }

    struct timeval tv;
    struct tm tm;
    if (getCurrentTime(tv, tm)) {
        tm.tm_hour = h;
        tm.tm_min = m;
        tm.tm_sec = s;
        if (setSystemTime(&tm)) {
            Console::printf("OK: Time set to %02d:%02d:%02d\r\n", h, m, s);
            if (s_timeCallback) {
                s_timeCallback();
            }
        } else {
            Console::printf("ERROR: Failed to set time\r\n");
        }
    } else {
        Console::printf("ERROR: Failed to set time\r\n");
    }
}

/**
 * \brief Updates the system clock date component.
 * \param args Date string in format `DD.MM.YYYY`.
 */
static void cmdSetDate(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: SET_DATE DD.MM.YYYY\r\n");
        return;
    }
    int d, m, y;
    if (sscanf(args, "%d.%d.%d", &d, &m, &y) != 3) {
        Console::printf("ERROR: Invalid format. Use DD.MM.YYYY\r\n");
        return;
    }
    if (d < 1 || d > 31 || m < 1 || m > 12 || y < YEAR_MIN || y > YEAR_MAX) {
        Console::printf("ERROR: Invalid date values\r\n");
        return;
    }

    struct timeval tv;
    struct tm tm;
    if (getCurrentTime(tv, tm)) {
        tm.tm_mday = d;
        tm.tm_mon = m - 1;
        tm.tm_year = y - 1900;
        if (setSystemTime(&tm)) {
            Console::printf("OK: Date set to %02d.%02d.%04d\r\n", d, m, y);
            if (s_timeCallback) {
                s_timeCallback();
            }
        } else {
            Console::printf("ERROR: Failed to set date\r\n");
        }
    } else {
        Console::printf("ERROR: Failed to set date\r\n");
    }
}

/**
 * \brief Display text command handlers.
 */

/**
 * \brief Sets the lock-screen name text through the text callback.
 * \param args New name string.
 */
static void cmdSetName(const char* args) {
    if (!args) args = "";
    if (s_textCallback) {
        s_textCallback("name", args);
    }
    Console::printf("OK: Name set to \"%s\"\r\n", args);
}

/**
 * \brief Sets the first info line through the text callback.
 * \param args New info text.
 */
static void cmdSetInfo(const char* args) {
    if (!args) args = "";
    if (s_textCallback) {
        s_textCallback("info", args);
    }
    Console::printf("OK: Info set to \"%s\"\r\n", args);
}

/**
 * \brief Sets the second info line through the text callback.
 * \param args New secondary info text.
 */
static void cmdSetInfo2(const char* args) {
    if (!args) args = "";
    if (s_textCallback) {
        s_textCallback("info2", args);
    }
    Console::printf("OK: Info2 set to \"%s\"\r\n", args);
}

/**
 * \brief Authentication command handlers for secure serial mode.
 */

#if FEATURE_SECURE_SERIAL
/**
 * \brief Authenticates the serial session with the supplied PIN.
 * \param args PIN string.
 */
static void cmdAuth(const char* args) {
    auto& pm = core::PinManager::instance();

    if (pm.isBadgeBlocked()) {
        if (pm.isLockoutActive()) {
            uint32_t remainingSec = pm.getLockoutRemainingMs() / 1000;
            Console::printf("ERROR: PIN locked. Wait %lu seconds.\r\n", (unsigned long)remainingSec);
        } else {
            Console::printf("ERROR: PIN permanently locked.\r\n");
        }
        return;
    }

    if (!args || !*args) {
        // No PIN argument: treat as logout.
        if (SerialCmd::isAuthenticated()) {
            SerialCmd::logout();
            Console::printf("OK: Logged out\r\n");
        } else {
            Console::printf("Usage: AUTH <pin>\r\n");
            Console::printf("Retries: %d\r\n", pm.getBadgeRetries());
        }
        return;
    }

    if (SerialCmd::authenticate(args)) {
        Console::printf("OK: Authenticated\r\n");
    } else {
        // Wrong PIN drops any active session so the next command runs
        // unprivileged instead of inheriting the previous session.
        if (SerialCmd::isAuthenticated()) {
            SerialCmd::logout();
        }
        uint8_t retries = pm.getBadgeRetries();
        if (retries == 0) {
            if (pm.isLockoutActive()) {
                uint32_t remainingSec = pm.getLockoutRemainingMs() / 1000;
                Console::printf("ERROR: Wrong PIN. Locked for %lu seconds.\r\n", (unsigned long)remainingSec);
            } else {
                Console::printf("ERROR: Wrong PIN. Permanently locked.\r\n");
            }
        } else {
            Console::printf("ERROR: Wrong PIN. %d retries remaining.\r\n", retries);
        }
    }
}

/**
 * \brief Logs out and clears the authenticated serial session state.
 * \param args Unused command arguments.
 */
static void cmdLogout(const char* args) {
    (void)args;
    SerialCmd::logout();
    Console::printf("OK: Logged out\r\n");
}
#endif

/**
 * \brief PIN state inspection and maintenance handlers.
 */

/**
 * \brief Resets badge PIN retry counters for debugging.
 * \param args Unused command arguments.
 */
static void cmdPinReset(const char* args) {
    (void)args;
    core::PinManager::instance().resetBadgeRetries();
    Console::printf("OK: Badge PIN retries reset to %d\r\n",
                    core::PinManager::instance().getBadgeRetries());
}

/**
 * \brief Prints the current badge PIN status snapshot.
 * \param args Unused command arguments.
 */
static void cmdPinStatus(const char* args) {
    (void)args;
    auto& pm = core::PinManager::instance();
    Console::printf("Badge PIN: retries=%d blocked=%s set=%s\r\n",
                    pm.getBadgeRetries(),
                    pm.isBadgeBlocked() ? "yes" : "no",
                    pm.isPinSet() ? "yes" : "no");
}

/**
 * \brief TROPIC01 secure-element maintenance and diagnostic handlers.
 */

/**
 * \brief Prints secure-element session status.
 * \param args Unused command arguments.
 */
static void cmdTr01Status(const char* args) {
    (void)args;
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    Console::printf("TR01 Status:\r\n");
    Console::printf("  Session: %s\r\n", se->isSessionActive() ? "active" : "inactive");
}

/**
 * \brief Prints secure-element chip and firmware information.
 * \param args Unused command arguments.
 */
static void cmdTr01Info(const char* args) {
    (void)args;
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    uint8_t chipId[8];
    uint8_t riscvVer[4] = {0};
    uint8_t spectVer[4] = {0};

    Console::printf("TR01 Info:\r\n");

    if (se->getChipId(chipId, sizeof(chipId))) {
        Console::printf("  Chip ID: ");
        for (int i = 0; i < 8; i++) {
            Console::printf("%02X", chipId[i]);
        }
        Console::printf("\r\n");
    } else {
        Console::printf("  Chip ID: (read failed)\r\n");
    }

    if (se->getFwVersion(riscvVer, spectVer)) {
        Console::printf("  RISC-V FW: v%u.%u.%u (build %u)\r\n",
                        riscvVer[3], riscvVer[2], riscvVer[1], riscvVer[0]);
        Console::printf("  SPECT FW:  v%u.%u.%u (build %u)\r\n",
                        spectVer[3], spectVer[2], spectVer[1], spectVer[0]);
    } else {
        Console::printf("  FW Version: (read failed)\r\n");
    }
}

/**
 * \brief Starts a secure-element session, restarting it if already active.
 * \param args Unused command arguments.
 */
static void cmdTr01Session(const char* args) {
    (void)args;
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    if (se->isSessionActive()) {
        Console::printf("Session already active, reconnecting...\r\n");
        se->sessionEnd();
    }

    if (se->sessionStart()) {
        Console::printf("OK: Session started\r\n");
    } else {
        Console::printf("ERROR: Session start failed\r\n");
    }
}

/**
 * \brief Prints usage information for ECC and R-Memory slots.
 * \param args Unused command arguments.
 */
static void cmdTr01Slots(const char* args) {
    (void)args;
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    Console::printf("ECC Key Slots (0-31):\r\n");
    int eccCount = 0;
    for (uint8_t i = 0; i < hal::ISecureElement::ECC_SLOT_COUNT; i++) {
        if (se->eccSlotUsed(i)) {
            Console::printf("  [%02d] Used\r\n", i);
            eccCount++;
        }
    }
    if (eccCount == 0) {
        Console::printf("  (none)\r\n");
    }

    Console::printf("\r\nR-Memory Slots:\r\n");
    Console::printf("  Slot 0:    System PIN/lockout\r\n");
    cdc::core::TropicSlotMap::instance().forEachRange(
        cdc::core::TropicSlotMap::SlotType::RMEM,
        [](const cdc::core::TropicSlotMap::SlotRange& r, void*) {
            Console::printf("  %4u-%4u: %s\r\n", r.start, r.end,
                            r.moduleName ? r.moduleName : "?");
        },
        nullptr);
}

/**
 * \brief Reads and dumps one secure-element R-Memory slot.
 * \param args Slot number argument.
 */
static void cmdTr01RmemRead(const char* args) {
    auto result = parseSlotArg(args, hal::ISecureElement::RMEM_SLOT_COUNT, "R-Memory slot");
    if (!result.valid) {
        Console::printf("Usage: TR01_RMEM_READ <slot>\r\n");
        return;
    }

    auto* se = getSecureElementWithCheck();
    if (!se) return;

    uint16_t slot = static_cast<uint16_t>(result.value);
    uint8_t data[256];
    uint16_t actualLen = 0;

    hal::SeResult seResult = se->rmemRead(slot, data, sizeof(data), &actualLen);
    if (seResult != hal::SeResult::OK) {
        Console::printf("ERROR: Read failed (slot may be empty)\r\n");
        return;
    }

    Console::printf("R-Memory Slot %d (%d bytes):\r\n", slot, actualLen);
    printHexDump(data, actualLen, actualLen);
}

/**
 * \brief Deletes one ECC key slot.
 * \param args ECC slot number argument.
 */
static void cmdTr01EccDel(const char* args) {
    auto result = parseSlotArg(args, hal::ISecureElement::ECC_SLOT_COUNT, "ECC slot");
    if (!result.valid) {
        Console::printf("Usage: TR01_ECC_DEL <slot>\r\n");
        return;
    }

    auto* se = getSecureElementWithCheck();
    if (!se) return;

    uint8_t slot = static_cast<uint8_t>(result.value);
    hal::SeResult seResult = se->eccDelete(slot);
    if (seResult == hal::SeResult::OK) {
        Console::printf("OK: ECC slot %d deleted\r\n", slot);
    } else {
        Console::printf("ERROR: Delete failed\r\n");
    }
}

/**
 * \brief Erases one R-Memory slot.
 * \param args R-Memory slot number argument.
 */
static void cmdTr01RmemDel(const char* args) {
    auto result = parseSlotArg(args, hal::ISecureElement::RMEM_SLOT_COUNT, "R-Memory slot");
    if (!result.valid) {
        Console::printf("Usage: TR01_RMEM_DEL <slot>\r\n");
        return;
    }

    auto* se = getSecureElementWithCheck();
    if (!se) return;

    uint16_t slot = static_cast<uint16_t>(result.value);
    hal::SeResult seResult = se->rmemErase(slot);
    if (seResult == hal::SeResult::OK) {
        Console::printf("OK: R-Memory slot %d erased\r\n", slot);
    } else {
        Console::printf("ERROR: Erase failed\r\n");
    }
}

/**
 * \brief Restarts the secure-element session to resynchronize state.
 * \param args Unused command arguments.
 */
static void cmdTr01Resync(const char* args) {
    (void)args;
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    Console::printf("Resyncing TR01 session...\r\n");

    if (se->isSessionActive()) {
        se->sessionEnd();
    }

    if (se->sessionStart()) {
        Console::printf("OK: Session restarted, cache invalidated\r\n");
    } else {
        Console::printf("ERROR: Session restart failed\r\n");
    }
}

/**
 * \brief Rebuilds the Tropic slot cache and prints per-slot diagnostics.
 * \param args Unused command arguments.
 */
static void cmdTr01CacheRebuild(const char* args) {
    (void)args;
    auto& storage = core::TropicStorage::instance();
    Console::printf("Rebuilding TR01 cache...\r\n");

    auto logFn = [](uint16_t slot, const char* message, void* ctx) {
        (void)ctx;
        if (!message) return;
        if (strcmp(message, "invalid header") == 0 ||
            strcmp(message, "mismatched module") == 0 ||
            strcmp(message, "nvs write failed") == 0 ||
            strcmp(message, "session start failed") == 0 ||
            strcmp(message, "read failed") == 0) {
            Console::printf("  slot %u: %s\r\n", slot, message);
        } else {
            Console::printf("  slot %u: found %s\r\n", slot, message);
        }
    };

    if (storage.rebuildVerbose(logFn, nullptr)) {
        Console::printf("OK: Cache rebuilt\r\n");
    } else {
        Console::printf("ERROR: Cache rebuild failed\r\n");
    }
}

/**
 * \brief Cleans up slot metadata inconsistencies and rebuilds cache state.
 * \param args Unused command arguments.
 */
static void cmdTr01Cleanup(const char* args) {
    (void)args;
    auto& storage = core::TropicStorage::instance();
    Console::printf("Cleaning TR01 cache + slots...\r\n");
    if (storage.cleanup()) {
        Console::printf("OK: Cleanup complete\r\n");
    } else {
        Console::printf("ERROR: Cleanup failed\r\n");
    }
}

/**
 * \brief Performs a destructive secure-element factory wipe after confirmation.
 * \param args Confirmation argument (`CONFIRM` required).
 */
static void cmdTr01Wipe(const char* args) {
    auto* se = getSecureElementWithCheck();
    if (!se) return;

    if (!args || strcmp(args, "CONFIRM") != 0) {
        Console::printf("WARNING: This will ERASE ALL data on TROPIC01!\r\n");
        Console::printf("  - All ECC keys (slots 0-31)\r\n");
        Console::printf("  - All R-Memory data (slots 0-511)\r\n");
        Console::printf("\r\nTo proceed, type: TR01_WIPE CONFIRM\r\n");
        return;
    }

    Console::printf("=== TROPIC01 Factory Reset ===\r\n");
    Console::flush();

    if (!se->isSessionActive()) {
        if (!se->sessionStart()) {
            Console::printf("ERROR: Cannot start session\r\n");
            return;
        }
    }

    Console::printf("Erasing ECC keys and R-Memory (this may take a while)...\r\n");
    Console::flush();
    auto result = core::wipeTropic(se, WIPE_PROGRESS_INTERVAL,
        [](uint16_t current, uint16_t total) {
            Console::printf("  Progress: %d/%d\r\n", current, total);
            Console::flush();
        });

    if (!result.sessionReady) {
        Console::printf("ERROR: SE session unavailable\r\n");
        return;
    }

    Console::printf("\r\n=== Factory Reset Complete ===\r\n");
    Console::printf("Deleted: %d ECC keys, %d R-Memory slots\r\n",
                    result.eccDeleted, result.rmemDeleted);
}

/**
 * \brief Public `SerialCmd` interface implementation.
 */

/**
 * \brief Initializes the serial console and registers built-in commands.
 */
void SerialCmd::init() {
    if (s_initialized) return;

    Console::init();

#if FEATURE_SECURE_SERIAL
    getCommandRegistry().setAuthProvider(isAuthenticated);
    getCommandRegistry().setOnCommandExecuted(resetAuthTimer);
#if !DEBUG_MODE
    // Release profile: suppress INFO/DEBUG/VERBOSE log output until a session
    // is authenticated. ERROR/WARN keep flowing so boot failures are still
    // visible.
    log_register_authgate_hook(SerialCmd::isAuthenticated);
#endif
#else
    log_set_level(CDC_LOG_LEVEL_DEBUG);
#endif

    registerBuiltinCommands();

    s_initialized = true;
    LOG_I(TAG, "Serial command processor initialized");

    Console::printf("\r\n=== CDC Badge OS Serial Console ===\r\n");
#if FEATURE_SECURE_SERIAL
    Console::printf("Login with: AUTH <pin>\r\n");
#endif
    Console::printf("Type 'HELP' for available commands.\r\n");
    Console::showPrompt();
}

/**
 * \brief Replaces the current input line with a history entry.
 *
 * Navigates the command history ring buffer in either direction and updates
 * the visible console line accordingly. No-op when the requested direction
 * has no further entries available.
 *
 * \param dir Navigation direction (older = arrow up, newer = arrow down).
 */
void SerialCmd::handleHistoryNav(HistoryDirection dir) {
    if (dir == HistoryDirection::OLDER) {
        if (s_historyPos >= s_historyCount) return;
        const char* hist = historyGet(s_historyPos);
        if (hist) {
            redrawLine(hist, s_cmdBufferPos);
            s_historyPos++;
        }
        return;
    }

    // HistoryDirection::NEWER
    if (s_historyPos == 0) return;
    s_historyPos--;
    if (s_historyPos == 0) {
        redrawLine("", s_cmdBufferPos);
        return;
    }
    const char* hist = historyGet(s_historyPos - 1);
    if (hist) {
        redrawLine(hist, s_cmdBufferPos);
    }
}

/**
 * \brief Processes one input character while in an active escape sequence.
 *
 * Implements a small state machine for ANSI CSI sequences:
 * `ESC` followed by `[` enters bracket mode, where `A`/`B` map to history
 * navigation. Any other character ends the sequence without action.
 *
 * \param c Input character.
 * \return `true` if the character was consumed by escape handling and the
 *         caller should not process it further; `false` otherwise.
 */
bool SerialCmd::handleEscape(int c) {
    if (s_escState == EscState::ESC) {
        if (c == '[') {
            s_escState = EscState::BRACKET;
        } else {
            s_escState = EscState::NONE;
        }
        return true;
    }

    if (s_escState == EscState::BRACKET) {
        s_escState = EscState::NONE;
        switch (c) {
            case 'A':
                handleHistoryNav(HistoryDirection::OLDER);
                break;
            case 'B':
                handleHistoryNav(HistoryDirection::NEWER);
                break;
            default:
                break;
        }
        return true;
    }

    return false;
}

/**
 * \brief Processes one input character against the special-key dispatch table.
 *
 * Handles control characters (ESC, CR/LF, backspace, Ctrl-C, Ctrl-U) and
 * printable characters. Mutates the command buffer and console output as
 * appropriate.
 *
 * \param c Input character.
 * \param[out] commandReady Set to `true` when a complete line was submitted.
 */
void SerialCmd::handleSpecialChar(int c, bool& commandReady) {
    commandReady = false;

    switch (c) {
        case 0x1B:  // ESC
            s_escState = EscState::ESC;
            return;

        case '\r':
        case '\n':
            Console::print("\r\n");
            s_cmdBuffer[s_cmdBufferPos] = '\0';
            if (s_cmdBufferPos > 0) {
                historyAdd(s_cmdBuffer);
                executeCommand(s_cmdBuffer);
            }
            s_cmdBufferPos = 0;
            s_historyPos = 0;
            Console::showPrompt();
            commandReady = true;
            return;

        case 0x7F:  // Backspace (DEL)
        case 0x08:  // Backspace (BS)
            if (s_cmdBufferPos > 0) {
                s_cmdBufferPos--;
                Console::print("\b \b");
            }
            return;

        case 0x03:  // Ctrl+C
            Console::print("^C\r\n");
            s_cmdBufferPos = 0;
            s_historyPos = 0;
            Console::showPrompt();
            return;

        case 0x15:  // Ctrl+U
            while (s_cmdBufferPos > 0) {
                Console::print("\b \b");
                s_cmdBufferPos--;
            }
            return;

        default: {
            static uint8_t utf8Pending = 0;
            static uint32_t utf8Cp = 0;

            auto appendByte = [](uint8_t b) {
                if (s_cmdBufferPos < CMD_BUFFER_SIZE - 1) {
                    s_cmdBuffer[s_cmdBufferPos++] = static_cast<char>(b);
                    Console::putchar(static_cast<char>(b));
                }
            };

            if (utf8Pending) {
                if ((c & 0xC0) == 0x80) {
                    utf8Cp = (utf8Cp << 6) | (c & 0x3F);
                    if (--utf8Pending == 0) {
                        uint8_t cp437 = cdc::ui::render::unicodeToCp437(utf8Cp);
                        if (cp437) appendByte(cp437);
                    }
                } else {
                    utf8Pending = 0;
                }
                return;
            }

            if ((c & 0xE0) == 0xC0) {
                utf8Cp = c & 0x1F;
                utf8Pending = 1;
                return;
            }
            if ((c & 0xF0) == 0xE0) {
                utf8Cp = c & 0x0F;
                utf8Pending = 2;
                return;
            }
            if ((c & 0xF8) == 0xF0) {
                utf8Cp = c & 0x07;
                utf8Pending = 3;
                return;
            }

            if (c >= 0x20 && c < 0x7F) {
                appendByte(static_cast<uint8_t>(c));
            } else if (c >= 0x80 && c <= 0xFF) {
                // Raw CP437/Latin-1 byte from terminals that don't speak UTF-8.
                appendByte(static_cast<uint8_t>(c));
            }
            return;
        }
    }
}

/**
 * \brief Processes one pending input character from the serial console.
 * \return `true` if a command line was completed, otherwise `false`.
 */
bool SerialCmd::process() {
    int c = Console::getchar();
    if (c < 0) return false;

    if (handleEscape(c)) return false;

    bool commandReady = false;
    handleSpecialChar(c, commandReady);
    return commandReady;
}

/**
 * \brief Returns the shared command registry instance.
 * \return Reference to the command registry.
 */
ICommandRegistry& SerialCmd::getRegistry() {
    return getCommandRegistry();
}

/**
 * \brief Sets the callback used by text-setting commands.
 * \param callback Callback receiving field key and new value.
 */
void SerialCmd::setTextCallback(TextChangeCallback callback) {
    s_textCallback = callback;
}

/**
 * \brief Sets the callback invoked after successful date/time updates.
 * \param callback Callback triggered on time change.
 */
void SerialCmd::setTimeCallback(TimeChangeCallback callback) {
    s_timeCallback = callback;
}

/**
 * \brief Returns whether the serial session is currently authenticated.
 * \return `true` when authenticated (and not timed out), otherwise `false`.
 */
bool SerialCmd::isAuthenticated() {
#if FEATURE_SECURE_SERIAL
    if (!s_authenticated) return false;

    uint64_t now = esp_timer_get_time();
    if ((now - s_authTimestamp) > (AUTH_TIMEOUT_MS * 1000ULL)) {
        s_authenticated = false;
#if !DEBUG_MODE
        log_set_level(CDC_LOG_LEVEL_WARN);
#endif
        LOG_I(TAG, "Session timed out");
        return false;
    }

    return true;
#else
    return true;
#endif
}

/**
 * \brief Attempts to authenticate the serial session with a PIN.
 * \param pin Candidate PIN string.
 * \return `true` on successful authentication, otherwise `false`.
 */
bool SerialCmd::authenticate(const char* pin) {
    auto& pm = core::PinManager::instance();

    if (pm.isBadgeBlocked()) {
        if (pm.isLockoutActive()) {
            uint32_t remainingSec = pm.getLockoutRemainingMs() / 1000;
            LOG_W(TAG, "PIN locked, %lu seconds remaining", (unsigned long)remainingSec);
        } else {
            LOG_W(TAG, "PIN permanently blocked (retries exhausted)");
        }
        return false;
    }

    if (!pin || !*pin) {
        LOG_W(TAG, "Empty PIN provided");
        return false;
    }

    if (!pm.verifyBadgePin(pin)) {
        LOG_W(TAG, "Authentication failed, %d retries remaining", pm.getBadgeRetries());
        return false;
    }

    s_authenticated = true;
    s_authTimestamp = esp_timer_get_time();
#if !DEBUG_MODE
    log_set_level(CDC_LOG_LEVEL_DEBUG);
#endif
    LOG_I(TAG, "Authenticated via serial");
    return true;
}

/**
 * \brief Logs out the current serial session.
 */
void SerialCmd::logout() {
    s_authenticated = false;
    s_authTimestamp = 0;
#if !DEBUG_MODE
    log_set_level(CDC_LOG_LEVEL_WARN);
#endif
    LOG_I(TAG, "Logged out");
}

/**
 * \brief Normalizes and dispatches a command line to the registry.
 * \param cmd Mutable command buffer.
 */
void SerialCmd::executeCommand(char* cmd) {
    cmd = trim(cmd);
    if (!*cmd) return;

    char first_token[24];
    size_t i = 0;
    while (cmd[i] && !isspace(static_cast<unsigned char>(cmd[i])) &&
           i < sizeof(first_token) - 1) {
        first_token[i] = cmd[i];
        i++;
    }
    first_token[i] = '\0';
    LOG_D(TAG, "Executing: %s", first_token);
    getCommandRegistry().processCommand(cmd);
}

/**
 * \brief Trims leading and trailing ASCII whitespace in-place.
 * \param str Mutable string pointer.
 * \return Pointer to the first non-space character inside `str`.
 */
char* SerialCmd::trim(char* str) {
    if (!str) return str;

    while (*str && isspace(static_cast<unsigned char>(*str))) str++;
    if (*str == '\0') return str;

    char* end = str + strlen(str) - 1;
    while (end > str && isspace(static_cast<unsigned char>(*end))) end--;
    *(end + 1) = '\0';

    return str;
}

/**
 * \brief Registers all built-in serial commands.
 */
void SerialCmd::registerBuiltinCommands() {
    auto& reg = getCommandRegistry();

    // System commands
    reg.registerCommand({"HELP", "Show available commands", cmdHelp, "system", false});
    reg.registerCommand({"PING", "Check if device is responsive", cmdPing, "system", false});
    reg.registerCommand({"STATUS", "Show system status", cmdStatus, "system", false});
    reg.registerCommand({"MEM", "Show memory usage", cmdMem, "system", false});
    reg.registerCommand({"MEMINFO", "Show detailed memory + task info", cmdMemInfo, "system", false});
    reg.registerCommand({"ERROR_LOG", "Show error log (CLEAR to reset)", cmdErrorLog, "system", false});
    reg.registerCommand({"REBOOT", "Restart the device", cmdReboot, "system", true});

    // NVS commands
    reg.registerCommand({"NVS_LIST", "List NVS entries [namespace]", cmdNvsList, "nvs", false});
    reg.registerCommand({"NVS_READ", "Read NVS key (ns key)", cmdNvsRead, "nvs", false});
    reg.registerCommand({"NVS_DEL", "Delete NVS key/namespace", cmdNvsDel, "nvs", true});
    reg.registerCommand({"NVS_CLEAR", "Erase entire NVS (NVS_CLEAR YES)", cmdNvsClear, "nvs", true});

    // Time commands
    reg.registerCommand({"GET_TIME", "Show current time", cmdGetTime, "time", false});
    reg.registerCommand({"GET_DATE", "Show current date", cmdGetDate, "time", false});
    reg.registerCommand({"SET_TIME", "Set time (HH:MM:SS)", cmdSetTime, "time", false});
    reg.registerCommand({"SET_DATE", "Set date (DD.MM.YYYY)", cmdSetDate, "time", false});

    // Display commands
    reg.registerCommand({"SET_NAME", "Set display name", cmdSetName, "display", false});
    reg.registerCommand({"SET_INFO", "Set info line 1", cmdSetInfo, "display", false});
    reg.registerCommand({"SET_INFO2", "Set info line 2", cmdSetInfo2, "display", false});

    // PIN debug commands
    reg.registerCommand({"PIN_STATUS", "Show PIN status", cmdPinStatus, "pin", false});
    reg.registerCommand({"PIN_RESET", "Reset PIN retries (debug)", cmdPinReset, "pin", false});

    // TROPIC01 Secure Element commands
    reg.registerCommand({"TR01_STATUS", "Show TR01 status", cmdTr01Status, "tr01", false});
    reg.registerCommand({"TR01_INFO", "Show TR01 chip info", cmdTr01Info, "tr01", false});
    reg.registerCommand({"TR01_SESSION", "Start/restart TR01 session", cmdTr01Session, "tr01", false});
    reg.registerCommand({"TR01_SLOTS", "Show TR01 slot usage", cmdTr01Slots, "tr01", false});
    reg.registerCommand({"TR01_RMEM_READ", "Read R-Memory slot", cmdTr01RmemRead, "tr01", false});
    reg.registerCommand({"TR01_ECC_DEL", "Delete ECC key slot", cmdTr01EccDel, "tr01", true});
    reg.registerCommand({"TR01_RMEM_DEL", "Delete R-Memory slot", cmdTr01RmemDel, "tr01", true});
    reg.registerCommand({"TR01_RESYNC", "Resync TR01 session and cache", cmdTr01Resync, "tr01", false});
    reg.registerCommand({"TR01_CACHE_REBUILD", "Rebuild TR01 cache", cmdTr01CacheRebuild, "tr01", false});
    reg.registerCommand({"TR01_CLEANUP", "Cleanup mismatched slots + rebuild cache", cmdTr01Cleanup, "tr01", true});
    reg.registerCommand({"TR01_WIPE", "Factory reset (TR01_WIPE CONFIRM)", cmdTr01Wipe, "tr01", true});

#if FEATURE_SECURE_SERIAL
    // Authentication commands
    reg.registerCommand({"AUTH", "Authenticate with PIN", cmdAuth, "auth", false});
    reg.registerCommand({"LOGOUT", "End authenticated session", cmdLogout, "auth", false});
#endif

    // WiFi commands
    registerWifiCommands();
}

} // namespace cdc::serial
