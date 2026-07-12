/**
 * Serial Command Processor Implementation
 * Handles input buffering, line editing, and command dispatch
 */

#include "serial_cmd/SerialCmd.h"
#include "serial_cmd/Console.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "cdc_core/feature_flags.h"
#include "cdc_core/Cp437.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/UsbServiceManager.h"
#include "cdc_core/PinManager.h"
#include "cdc_core/TropicSlotMap.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/FactoryReset.h"
#include "cdc_core/CpuStats.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_os_ui/AppUi.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_os_ui/FirmwareCheck.h"
#include "cdc_log.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_views/T9InputView.h"
#include "cdc_ui/ViewStack.h"
#include "plugin_manager/host_api.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
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
 * \brief Prints the firmware version, the plugin host API level and the last
 *        upstream firmware-check result (if any).
 * \param args Unused command arguments.
 */
static void cmdVersion(const char* args) {
    (void)args;
    Console::printf("Firmware: %s\r\n", APP_VERSION);
    Console::printf("API level: %s\r\n", HOST_API_LEVEL_STR);
    // Build profile: lets the provisioning tool verify a release build
    // (debug off, secure serial on) before it burns any lockdown eFuses.
    // flash_enc/secure_boot are RUNTIME eFuse states, not build flags, so the
    // tool can confirm the lockdown actually took effect on this chip.
    // pairing_slot is the SH0 slot the firmware authenticates with, required
    // by rotate-key --verify before the old slot may be invalidated.
    int pairingSlot = -1;
    if (auto* se = hal::getSecureElementInstance()) {
        pairingSlot = (int)se->activePairingSlot();
    }
    Console::printf("Profile: 0x%02X debug=%d secure_serial=%d provisioning=%d "
                    "pairing_slot=%d flash_enc=%d secure_boot=%d\r\n",
                    (unsigned)BUILD_PROFILE_BYTE, (int)DEBUG_MODE,
                    (int)FEATURE_SECURE_SERIAL, (int)FEATURE_PROVISIONING,
                    pairingSlot, esp_flash_encryption_enabled() ? 1 : 0,
                    esp_secure_boot_enabled() ? 1 : 0);
    char last[64];
    if (cdc::ui::firmwareCheckLastResult(last, sizeof(last))) {
        Console::printf("Latest: %s\r\n", last);
    } else {
        Console::printf("Latest: not checked yet\r\n");
    }
    Console::flush();
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

static void cmdCpu(const char* args) {
    (void)args;
    Console::printf("Measuring CPU load (~250 ms)...\r\n");
    Console::flush();
    uint8_t load = cdc::core::CpuStats::loadOverWindow();
    Console::printf("CPU load: %u %%\r\n", (unsigned)load);
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
static void printHeapRegion(const char* label, uint32_t caps) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    if (info.total_free_bytes + info.total_allocated_bytes == 0) return;
    Console::printf("\r\n-- %s --\r\n", label);
    Console::printf("  total free      : %lu\r\n", (unsigned long)info.total_free_bytes);
    Console::printf("  total allocated : %lu\r\n", (unsigned long)info.total_allocated_bytes);
    Console::printf("  largest free    : %lu\r\n", (unsigned long)info.largest_free_block);
    Console::printf("  min ever free   : %lu\r\n", (unsigned long)info.minimum_free_bytes);
    Console::printf("  free blocks     : %lu\r\n", (unsigned long)info.free_blocks);
    Console::printf("  alloc blocks    : %lu\r\n", (unsigned long)info.allocated_blocks);
}

static void cmdMemInfo(const char* args) {
    (void)args;
    Console::printf("=== Detailed Memory Info ===\r\n");

    printHeapRegion("Internal (any)",       MALLOC_CAP_INTERNAL);
    printHeapRegion("Internal DRAM (8-bit)", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printHeapRegion("Internal 32-bit only",  MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    printHeapRegion("IRAM (executable)",     MALLOC_CAP_EXEC);
    printHeapRegion("DMA-capable",           MALLOC_CAP_DMA);
    printHeapRegion("PSRAM",                 MALLOC_CAP_SPIRAM);
    printHeapRegion("RTC slow RAM",          MALLOC_CAP_RTCRAM);

    Console::printf("\r\n-- Heap totals (heap_caps_get_total_size) --\r\n");
    Console::printf("  INTERNAL : %u\r\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    Console::printf("  EXEC     : %u\r\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_EXEC));
    Console::printf("  SPIRAM   : %u\r\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    Console::printf("  DMA      : %u\r\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_DMA));
    Console::printf("  INTERNAL|EXEC : free=%u largest=%u total=%u\r\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC),
                    (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC));
    Console::printf("  32BIT          : free=%u largest=%u\r\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_32BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_32BIT));

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
 * \brief Reboots the device into USB download (bootloader) mode.
 * \param args Unused command arguments.
 */
static void cmdBootloader(const char* args) {
    (void)args;
    Console::printf("Rebooting into download mode...\r\n");
    Console::flush();
    cdc::ui::rebootIntoBootloader();
}

/**
 * \brief Enters ship mode (disconnects the battery via BATFET).
 * \param args Unused command arguments.
 */
static void cmdShipMode(const char* args) {
    (void)args;
    auto* power = hal::getPowerManagerInstance();
    if (!power) {
        Console::printf("ERROR: Power manager not available\r\n");
        return;
    }
    Console::printf("Entering ship mode (battery disconnect)...\r\n");
    Console::flush();
    power->enterShipMode();
    Console::printf("OK\r\n");
}

static void cmdPaste(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: PASTE <text>\r\n");
        return;
    }
    cdc::ui::IView* top = cdc::ui::ViewStack::instance().current();
    if (!top || strcmp(top->getName(), "T9InputView") != 0) {
        Console::printf("ERROR: T9 input view not active\r\n");
        return;
    }
    auto* t9 = static_cast<cdc::ui::T9InputView*>(top);
    uint16_t added = t9->appendRaw(args);
    Console::printf("OK: %u chars\r\n", static_cast<unsigned>(added));
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
        Console::printf("\r\nTo proceed, type: NVS CLEAR YES\r\n");
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
        Console::printf("Usage: NVS READ <namespace> <key>\r\n");
        return;
    }

    char ns[NVS_NAMESPACE_MAX_LEN + 1] = {0};
    char key[NVS_KEY_MAX_LEN + 1] = {0};
    if (sscanf(args, "%15s %15s", ns, key) != 2) {
        Console::printf("Usage: NVS READ <namespace> <key>\r\n");
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
        Console::printf("Usage: NVS DEL <namespace> [key]\r\n");
        Console::printf("  Without key: erases entire namespace\r\n");
        return;
    }

    char ns[NVS_NAMESPACE_MAX_LEN + 1] = {0};
    char key[NVS_KEY_MAX_LEN + 1] = {0};
    int parsed = sscanf(args, "%15s %15s", ns, key);

    if (parsed < 1) {
        Console::printf("Usage: NVS DEL <namespace> [key]\r\n");
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
        Console::printf("Usage: SET_DATE DD.MM.YYYY | <unix_seconds>\r\n");
        return;
    }

    int d, m, y;
    if (sscanf(args, "%d.%d.%d", &d, &m, &y) == 3) {
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
                return;
            }
        }
        Console::printf("ERROR: Failed to set date\r\n");
        return;
    }

    // No dotted date: treat the argument as a Unix timestamp (UTC seconds)
    // and set the full clock (date and time of day) at once.
    long long ts;
    if (sscanf(args, "%lld", &ts) != 1 || ts < 0) {
        Console::printf("ERROR: Invalid format. Use DD.MM.YYYY or a Unix timestamp\r\n");
        return;
    }
    time_t secs = static_cast<time_t>(ts);
    struct tm tm;
    if (!gmtime_r(&secs, &tm)) {
        Console::printf("ERROR: Failed to set date\r\n");
        return;
    }
    int year = tm.tm_year + 1900;
    if (year < YEAR_MIN || year > YEAR_MAX) {
        Console::printf("ERROR: Timestamp out of range (%d-%d)\r\n", YEAR_MIN, YEAR_MAX);
        return;
    }
    struct timeval tv;
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) == 0) {
        Console::printf("OK: Time set to %lld (%04d-%02d-%02d %02d:%02d:%02d UTC)\r\n",
                        ts, year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (s_timeCallback) {
            s_timeCallback();
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
 * \brief Changes the badge PIN after verifying the current one.
 * \param args "<currentPin> <newPin>" - both must be 4..8 digits.
 */
static void cmdPinChange(const char* args) {
    if (!args || !args[0]) {
        Console::printf("Usage: PIN CHANGE <currentPin> <newPin>\r\n");
        return;
    }

    const char* space = strchr(args, ' ');
    if (!space) {
        Console::printf("Usage: PIN CHANGE <currentPin> <newPin>\r\n");
        return;
    }

    char currentPin[core::PinManager::BADGE_PIN_MAX + 1] = {};
    char newPin[core::PinManager::BADGE_PIN_MAX + 1] = {};

    size_t curLen = static_cast<size_t>(space - args);
    if (curLen == 0 || curLen > core::PinManager::BADGE_PIN_MAX) {
        Console::printf("ERROR: PIN length must be %u-%u digits\r\n",
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MIN),
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MAX));
        return;
    }
    memcpy(currentPin, args, curLen);
    currentPin[curLen] = '\0';

    const char* p = space + 1;
    while (*p == ' ') p++;
    size_t newLen = strlen(p);
    if (newLen == 0 || newLen > core::PinManager::BADGE_PIN_MAX) {
        Console::printf("ERROR: PIN length must be %u-%u digits\r\n",
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MIN),
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MAX));
        return;
    }
    memcpy(newPin, p, newLen);
    newPin[newLen] = '\0';

    auto isDigits = [](const char* s) {
        for (; *s; ++s) if (!isdigit(static_cast<unsigned char>(*s))) return false;
        return true;
    };
    if (!isDigits(currentPin) || !isDigits(newPin)) {
        Console::printf("ERROR: PIN must be digits only\r\n");
        return;
    }

    auto& pm = core::PinManager::instance();
    if (pm.isBadgeBlocked()) {
        Console::printf("ERROR: PIN entry blocked (lockout active or no retries left)\r\n");
        return;
    }

    if (!pm.changeBadgePin(currentPin, newPin)) {
        Console::printf("ERROR: PIN change failed (current PIN wrong or new PIN invalid)\r\n");
        return;
    }

    Console::printf("OK: PIN changed\r\n");
}

/**
 * \brief Arms the duress / self-destruct PIN.
 *
 * Entering the duress PIN at the lock screen wipes all data and reboots.
 * The duress PIN must be 4-8 digits and differ from the badge PIN.
 *
 * \param args "<duressPin>".
 */
static void cmdPinDuress(const char* args) {
    if (!args || !args[0]) {
        Console::printf("Usage: PIN DURESS <pin>\r\n");
        return;
    }

    char duressPin[core::PinManager::BADGE_PIN_MAX + 1] = {};
    size_t len = strlen(args);
    if (len == 0 || len > core::PinManager::BADGE_PIN_MAX) {
        Console::printf("ERROR: PIN length must be %u-%u digits\r\n",
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MIN),
                        static_cast<unsigned>(core::PinManager::BADGE_PIN_MAX));
        return;
    }
    memcpy(duressPin, args, len);
    duressPin[len] = '\0';

    if (!core::PinManager::instance().setDuressPin(duressPin)) {
        Console::printf("ERROR: Duress PIN rejected (invalid length, non-digit, or equal to badge PIN)\r\n");
        return;
    }

    Console::printf("OK: Duress PIN armed\r\n");
}

/**
 * \brief Disarms the duress / self-destruct PIN.
 * \param args Unused command arguments.
 */
static void cmdPinDuressClear(const char* args) {
    (void)args;
    core::PinManager::instance().clearDuressPin();
    Console::printf("OK: Duress PIN cleared\r\n");
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
        Console::printf("Usage: TR01 RMEM_READ <slot>\r\n");
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
        Console::printf("Usage: TR01 ECC_DEL <slot>\r\n");
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
        Console::printf("Usage: TR01 RMEM_DEL <slot>\r\n");
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
        Console::printf("\r\nTo proceed, type: TR01 WIPE CONFIRM\r\n");
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

#if FEATURE_PROVISIONING
/**
 * \brief Decodes exactly `len` bytes from `2*len` hex characters.
 * \param hex Null-terminated hex string (no separators).
 * \param out Output buffer of `len` bytes.
 * \param len Expected byte count.
 * \return true on success, false if the string is the wrong length or contains
 *         a non-hex character.
 */
static bool decodeHexExact(const char* hex, uint8_t* out, size_t len) {
    if (!hex || !out) return false;
    size_t n = strlen(hex);
    if (n != len * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < len; ++i) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

/**
 * \brief Writes a host X25519 public key into an empty TROPIC01 pairing slot.
 * \param args "<slot> <64-hex-char pubkey>".
 *
 * One-shot per slot, irreversible. Used only by tools/provision.py for SH0
 * sync-key rotation. The driver rejects the active pairing slot.
 */
static void cmdTr01PairWrite(const char* args) {
    unsigned slot = 0;
    char hex[80] = {0};
    if (!args || sscanf(args, "%u %79s", &slot, hex) != 2) {
        Console::printf("Usage: TR01 PAIR_WRITE <slot 0-3> <64-hex-char pubkey>\r\n");
        return;
    }
    uint8_t pub[32];
    if (!decodeHexExact(hex, pub, sizeof(pub))) {
        Console::printf("ERROR: pubkey must be exactly 64 hex chars (32 bytes)\r\n");
        return;
    }

    auto* se = getSecureElementWithCheck();
    if (!se) return;

    hal::SeResult r = se->pairingKeyWrite(static_cast<uint8_t>(slot), pub);
    if (r == hal::SeResult::OK) {
        Console::printf("OK: pairing key written to slot %u\r\n", slot);
    } else if (r == hal::SeResult::INVALID_PARAM) {
        Console::printf("ERROR: invalid slot (0-3, not the active slot)\r\n");
    } else {
        Console::printf("ERROR: pairing write failed (slot occupied or no session)\r\n");
    }
}

/**
 * \brief PERMANENTLY invalidates a TROPIC01 pairing slot after confirmation.
 * \param args "<slot> CONFIRM".
 *
 * Irreversible: the slot is dead forever. Guarded by an explicit CONFIRM token
 * in addition to the tool-side gates; the driver rejects the active slot.
 */
static void cmdTr01PairInvalidate(const char* args) {
    unsigned slot = 0;
    char token[16] = {0};
    int n = args ? sscanf(args, "%u %15s", &slot, token) : 0;
    if (n < 1) {
        Console::printf("Usage: TR01 PAIR_INVALIDATE <slot 0-3> CONFIRM\r\n");
        return;
    }
    if (n < 2 || strcmp(token, "CONFIRM") != 0) {
        Console::printf("WARNING: PERMANENTLY invalidates pairing slot %u.\r\n", slot);
        Console::printf("  The slot is dead forever. Invalidating all 4 bricks the chip.\r\n");
        Console::printf("\r\nTo proceed, type: TR01 PAIR_INVALIDATE %u CONFIRM\r\n", slot);
        return;
    }

    auto* se = getSecureElementWithCheck();
    if (!se) return;

    hal::SeResult r = se->pairingKeyInvalidate(static_cast<uint8_t>(slot));
    if (r == hal::SeResult::OK) {
        Console::printf("OK: pairing slot %u permanently invalidated\r\n", slot);
    } else if (r == hal::SeResult::INVALID_PARAM) {
        Console::printf("ERROR: invalid slot (0-3, not the active slot)\r\n");
    } else {
        Console::printf("ERROR: pairing invalidate failed (no session)\r\n");
    }
}
#endif // FEATURE_PROVISIONING

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

    // Binary-streaming bypass: while a byte interceptor is installed, every
    // incoming byte is delivered raw with no echo, no buffering and no line
    // processing. Used by `PLUGIN UPLOAD` to slurp the raw payload.
    if (auto bi = getCommandRegistry().getByteInterceptor()) {
        bi(static_cast<uint8_t>(c));
        return;
    }

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
                        uint8_t cp437 = cdc::core::cp437::fromUnicode(utf8Cp);
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
    bool anyCommandReady = false;
    // Drain every byte that is currently buffered, not just one per main-loop
    // tick. Without this, binary uploads were rate-limited to one byte per
    // ~50 ms UI tick (~20 B/s).
    for (int i = 0; i < 4096; ++i) {
        int c = Console::getchar();
        if (c < 0) break;

        if (handleEscape(c)) continue;

        bool commandReady = false;
        handleSpecialChar(c, commandReady);
        if (commandReady) anyCommandReady = true;
    }
    return anyCommandReady;
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
 * \brief Keeps the auth session alive during a long-running serial activity.
 */
void SerialCmd::touchAuthSession() {
#if FEATURE_SECURE_SERIAL
    if (s_authenticated) {
        s_authTimestamp = esp_timer_get_time();
    }
#endif
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
 * \brief WiFi serial command handlers.
 *
 * Thin wrappers around \ref cdc::ui::WifiHandlers for credential storage and
 * connection lifecycle, plus \ref cdc::hal::IWifiController for runtime state
 * (scan, mode, IP/MAC/RSSI).
 */

static constexpr uint32_t WIFI_SCAN_POLL_MS     = 100;
static constexpr uint8_t  WIFI_MAX_SCAN_RESULTS = hal::IWifiController::MAX_SCAN_RESULTS;

static const char* wifiSecurityName(hal::WifiSecurity sec) {
    switch (sec) {
        case hal::WifiSecurity::OPEN:            return "OPEN";
        case hal::WifiSecurity::WEP:             return "WEP";
        case hal::WifiSecurity::WPA_PSK:         return "WPA";
        case hal::WifiSecurity::WPA2_PSK:        return "WPA2";
        case hal::WifiSecurity::WPA3_PSK:        return "WPA3";
        case hal::WifiSecurity::WPA2_ENTERPRISE: return "WPA2-E";
        default:                                 return "?";
    }
}

static const char* wifiStateName(hal::WifiState st) {
    switch (st) {
        case hal::WifiState::DISCONNECTED:      return "DISCONNECTED";
        case hal::WifiState::CONNECTING:        return "CONNECTING";
        case hal::WifiState::CONNECTED:         return "CONNECTED";
        case hal::WifiState::CONNECTION_FAILED: return "FAILED";
        case hal::WifiState::GOT_IP:            return "GOT_IP";
        default:                                return "?";
    }
}

static const char* wifiModeName(hal::WifiMode m) {
    switch (m) {
        case hal::WifiMode::OFF:    return "OFF";
        case hal::WifiMode::STA:    return "STA";
        case hal::WifiMode::AP:     return "AP";
        case hal::WifiMode::STA_AP: return "STA_AP";
        default:                    return "?";
    }
}

/**
 * \brief Deduplicates scan results by SSID (keeping strongest RSSI) and sorts
 *        the survivors descending by RSSI in place.
 */
static uint8_t wifiDedupAndSort(hal::WifiScanResult* results, uint8_t count) {
    if (count <= 1) return count;

    uint8_t unique = 0;
    for (uint8_t i = 0; i < count; i++) {
        bool seen = false;
        for (uint8_t j = 0; j < unique; j++) {
            if (strcmp(results[i].ssid, results[j].ssid) == 0) {
                seen = true;
                if (results[i].rssi > results[j].rssi) results[j] = results[i];
                break;
            }
        }
        if (!seen && results[i].ssid[0] != '\0') {
            if (unique != i) results[unique] = results[i];
            unique++;
        }
    }

    for (uint8_t i = 0; i < unique; i++) {
        for (uint8_t j = i + 1; j < unique; j++) {
            if (results[j].rssi > results[i].rssi) {
                hal::WifiScanResult tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }
    return unique;
}

/**
 * \brief WIFI_SCAN - scan and print networks (deduplicated, sorted by RSSI).
 */
static void cmdWifiScan(const char* args) {
    (void)args;

    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    if (!wifi->isEnabled() || wifi->getMode() == hal::WifiMode::AP) {
        if (!wifi->enable(hal::WifiMode::STA)) {
            Console::printf("ERROR: Failed to enable WiFi\r\n");
            return;
        }
    }

    Console::printf("Scanning...\r\n");

    if (!wifi->startScan()) {
        Console::printf("ERROR: Scan start failed\r\n");
        return;
    }

    uint32_t elapsed = 0;
    while (!wifi->isScanComplete() && elapsed < ui::WIFI_SCAN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_POLL_MS));
        elapsed += WIFI_SCAN_POLL_MS;
    }

    if (!wifi->isScanComplete()) {
        Console::printf("ERROR: Scan timeout\r\n");
        return;
    }

    hal::WifiScanResult results[WIFI_MAX_SCAN_RESULTS];
    uint8_t count = wifi->getScanResults(results, WIFI_MAX_SCAN_RESULTS);
    count = wifiDedupAndSort(results, count);

    if (count == 0) {
        Console::printf("No networks found\r\n");
    } else {
        Console::printf("#  %-32s %5s %3s %s\r\n", "SSID", "RSSI", "Ch", "Security");
        for (uint8_t i = 0; i < count; i++) {
            Console::printf("%-2u %-32s %4d %3u %s\r\n",
                            static_cast<unsigned>(i + 1),
                            results[i].ssid,
                            results[i].rssi,
                            static_cast<unsigned>(results[i].channel),
                            wifiSecurityName(results[i].security));
        }
    }

    Console::printf("OK\r\n");
}

/**
 * \brief WIFI_STATUS - show runtime state and saved configuration.
 */
static void cmdWifiStatus(const char* args) {
    (void)args;

    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    Console::printf("Mode:      %s\r\n", wifiModeName(wifi->getMode()));
    Console::printf("Enabled:   %s\r\n", wifi->isEnabled() ? "yes" : "no");
    Console::printf("State:     %s\r\n", wifiStateName(wifi->getWifiState()));

    if (wifi->isConnected()) {
        Console::printf("SSID:      %s\r\n", wifi->getCurrentSsid());

        char ip[16] = {};
        if (wifi->getIpAddress(ip, sizeof(ip))) {
            Console::printf("IP:        %s\r\n", ip);
        }
        uint8_t mac[6] = {};
        if (wifi->getMacAddress(mac)) {
            Console::printf("MAC:       %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        int8_t rssi = wifi->getRssi();
        if (rssi != 0) {
            Console::printf("RSSI:      %d dBm\r\n", rssi);
        }
    }

    auto& wh = ui::WifiHandlers::instance();
    wh.loadConfig();
    const auto& cfg = wh.config();
    if (cfg.valid) {
        Console::printf("\r\nSaved config:\r\n");
        Console::printf("SSID:      %s\r\n", cfg.ssid);
        Console::printf("Security:  %s\r\n",
                        wifiSecurityName(static_cast<hal::WifiSecurity>(cfg.security)));
        Console::printf("Timeout:   %lu ms\r\n",
                        static_cast<unsigned long>(wh.getConnectTimeoutMs()));
    } else {
        Console::printf("\r\nSaved config: (none)\r\n");
    }

    Console::printf("OK\r\n");
}

/**
 * \brief WIFI_ON [sta|ap|sta_ap] - enable WiFi radio.
 *
 * In STA modes, auto-reconnects via WifiHandlers when saved credentials exist.
 */
static void cmdWifiOn(const char* args) {
    hal::WifiMode mode = hal::WifiMode::STA;

    if (args && args[0]) {
        if (strcasecmp(args, "ap") == 0) {
            mode = hal::WifiMode::AP;
        } else if (strcasecmp(args, "sta_ap") == 0) {
            mode = hal::WifiMode::STA_AP;
        } else if (strcasecmp(args, "sta") != 0) {
            Console::printf("Usage: WIFI ON [sta|ap|sta_ap]\r\n");
            return;
        }
    }

    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    if (!wifi->enable(mode)) {
        Console::printf("ERROR: Failed to enable WiFi\r\n");
        return;
    }

    Console::printf("OK: %s mode enabled\r\n", wifiModeName(mode));

    if (mode == hal::WifiMode::AP) return;

    auto& wh = ui::WifiHandlers::instance();
    wh.loadConfig();
    if (!wh.config().valid) return;

    Console::printf("Reconnecting to %s...\r\n", wh.config().ssid);
    if (!wh.setUserEnabled(true)) {
        wifi->disable();
        const char* err = wh.getLastError();
        Console::printf("ERROR: Reconnect failed (%s)\r\n", err ? err : "?");
        return;
    }

    char ip[16] = {};
    wifi->getIpAddress(ip, sizeof(ip));
    Console::printf("OK: %s\r\n", ip[0] ? ip : "connected");
}

/**
 * \brief WIFI_OFF - disconnect and disable WiFi radio.
 */
static void cmdWifiOff(const char* args) {
    (void)args;

    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        Console::printf("ERROR: WiFi not available\r\n");
        return;
    }
    if (!wifi->isEnabled()) {
        Console::printf("OK: already off\r\n");
        return;
    }
    ui::WifiHandlers::instance().setUserEnabled(false);
    Console::printf("OK: WiFi disabled\r\n");
}

/**
 * \brief WIFI_CONNECT <ssid> <password> - connect and persist credentials.
 */
static void cmdWifiConnect(const char* args) {
    if (!args || !args[0]) {
        Console::printf("Usage: WIFI CONNECT <ssid> <password>\r\n");
        return;
    }

    const char* space = strchr(args, ' ');
    if (!space) {
        Console::printf("Usage: WIFI CONNECT <ssid> <password>\r\n");
        return;
    }

    char ssid[33] = {};
    char password[65] = {};

    size_t ssidLen = static_cast<size_t>(space - args);
    if (ssidLen >= sizeof(ssid)) ssidLen = sizeof(ssid) - 1;
    memcpy(ssid, args, ssidLen);
    ssid[ssidLen] = '\0';

    const char* pw = space + 1;
    while (*pw == ' ') pw++;
    size_t pwLen = strlen(pw);
    if (pwLen >= sizeof(password)) pwLen = sizeof(password) - 1;
    memcpy(password, pw, pwLen);
    password[pwLen] = '\0';

    if (ssid[0] == '\0') {
        Console::printf("ERROR: SSID required\r\n");
        return;
    }

    auto& wh = ui::WifiHandlers::instance();
    wh.saveCredentials(ssid, password);

    Console::printf("Connecting to %s (timeout: %lu ms)...\r\n",
                    ssid, static_cast<unsigned long>(wh.getConnectTimeoutMs()));

    if (!wh.setUserEnabled(true)) {
        const char* err = wh.getLastError();
        Console::printf("ERROR: Connection failed (%s)\r\n", err ? err : "?");
        return;
    }

    auto* wifi = hal::getWifiControllerInstance();
    char ip[16] = {};
    if (wifi) wifi->getIpAddress(ip, sizeof(ip));
    Console::printf("OK: %s\r\n", ip[0] ? ip : "connected");
}

/**
 * \brief WIFI_TIMEOUT [ms] - get or set the connect timeout.
 */
static void cmdWifiTimeout(const char* args) {
    auto& wh = ui::WifiHandlers::instance();
    if (!args || !args[0]) {
        Console::printf("%lu ms\r\n",
                        static_cast<unsigned long>(wh.getConnectTimeoutMs()));
        return;
    }

    char* end = nullptr;
    long val = strtol(args, &end, 10);
    if (end == args || *end != '\0'
        || val < static_cast<long>(ui::WIFI_CONNECT_TIMEOUT_MIN_MS)
        || val > static_cast<long>(ui::WIFI_CONNECT_TIMEOUT_MAX_MS)) {
        Console::printf("Usage: WIFI TIMEOUT [%lu-%lu]\r\n",
                        static_cast<unsigned long>(ui::WIFI_CONNECT_TIMEOUT_MIN_MS),
                        static_cast<unsigned long>(ui::WIFI_CONNECT_TIMEOUT_MAX_MS));
        return;
    }

    if (!wh.setConnectTimeoutMs(static_cast<uint32_t>(val))) {
        Console::printf("ERROR: Failed to persist timeout\r\n");
        return;
    }
    Console::printf("OK: %ld ms\r\n", val);
}

/**
 * \brief WIFI_FORGET - disable WiFi and erase the saved configuration.
 */
static void cmdWifiForget(const char* args) {
    (void)args;

    auto& wh = ui::WifiHandlers::instance();
    wh.disconnect();
    wh.clearConfig();

    Console::printf("OK: WiFi configuration cleared\r\n");
}

/**
 * \brief Module management serial command handlers.
 */

/**
 * \brief Finds a registered module index by name (case-insensitive).
 * \param name Module name to look up.
 * \return Module index, or -1 if not found.
 */
static int findModuleIndex(const char* name) {
    auto& reg = core::ModuleRegistry::instance();
    uint8_t count = reg.getModuleCount();
    for (uint8_t i = 0; i < count; i++) {
        core::IModule* module = reg.getModuleAt(i);
        if (module && module->getName() &&
            strcasecmp(module->getName(), name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * \brief MODULE LIST - list registered modules with state and errors.
 * \param args Unused command arguments.
 */
static void cmdModuleList(const char* args) {
    (void)args;
    auto& reg = core::ModuleRegistry::instance();
    uint8_t count = reg.getModuleCount();

    Console::printf("=== Modules (%u) ===\r\n", static_cast<unsigned>(count));
    for (uint8_t i = 0; i < count; i++) {
        core::IModule* module = reg.getModuleAt(i);
        if (!module) continue;

        const char* error = reg.getModuleSlotError(i);
        Console::printf("  [%2u] %-16s %-8s %-6s %s\r\n",
                        static_cast<unsigned>(i),
                        module->getName() ? module->getName() : "?",
                        reg.isModuleEnabled(i) ? "enabled" : "disabled",
                        reg.getModuleStatusLabel(i),
                        error ? error : "");
    }
}

/**
 * \brief MODULE ENABLE <name> - enable a module by name (persistent).
 * \param args Module name.
 */
static void cmdModuleEnable(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: MODULE ENABLE <name>\r\n");
        return;
    }

    int index = findModuleIndex(args);
    if (index < 0) {
        Console::printf("ERROR: Module '%s' not found\r\n", args);
        return;
    }

    auto& reg = core::ModuleRegistry::instance();
    uint8_t idx = static_cast<uint8_t>(index);

    if (reg.isModuleEnabled(idx)) {
        Console::printf("OK: Module '%s' already enabled\r\n", args);
        return;
    }

    bool needsReplugBefore = core::UsbManager::instance().needsReplug();

    reg.setModuleEnabled(idx, true);
    if (!reg.startModule(idx)) {
        switch (reg.classifyStartFailure(idx)) {
            case core::ModuleStartFailure::SlotError:
                Console::printf("ERROR: %s\r\n", reg.getModuleSlotError(idx));
                break;
            case core::ModuleStartFailure::UsbBudgetFull:
                Console::printf("ERROR: No free USB slot - disable a USB module (e.g. GPG) first\r\n");
                break;
            case core::ModuleStartFailure::Generic:
                Console::printf("ERROR: Failed to start module '%s'\r\n", args);
                break;
        }
        return;
    }

    Console::printf("OK: Module '%s' enabled\r\n", args);

    if (core::UsbManager::instance().newlyRequiresReplug(needsReplugBefore)) {
        Console::printf("NOTE: USB replug required\r\n");
    }
}

/**
 * \brief MODULE DISABLE <name> - disable a module by name (persistent).
 * \param args Module name.
 */
static void cmdModuleDisable(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: MODULE DISABLE <name>\r\n");
        return;
    }

    int index = findModuleIndex(args);
    if (index < 0) {
        Console::printf("ERROR: Module '%s' not found\r\n", args);
        return;
    }

    auto& reg = core::ModuleRegistry::instance();
    uint8_t idx = static_cast<uint8_t>(index);

    if (!reg.isModuleEnabled(idx)) {
        Console::printf("OK: Module '%s' already disabled\r\n", args);
        return;
    }

    bool needsReplugBefore = core::UsbManager::instance().needsReplug();

    reg.setModuleEnabled(idx, false);
    core::IModule* module = reg.getModuleAt(idx);
    if (module && module->getState() == core::ServiceState::STARTED) {
        module->stop();
    }

    Console::printf("OK: Module '%s' disabled\r\n", args);

    if (core::UsbManager::instance().newlyRequiresReplug(needsReplugBefore)) {
        Console::printf("NOTE: USB replug required\r\n");
    }
}

/**
 * \brief Human-readable name for a USB service state.
 */
static const char* usbSvcStateName(core::UsbServiceState state) {
    switch (state) {
        case core::UsbServiceState::On:        return "on";
        case core::UsbServiceState::Off:       return "off";
        case core::UsbServiceState::Suspended: return "suspended";
        case core::UsbServiceState::Unavailable:
        default:                               return "unavailable";
    }
}

/**
 * \brief USBSVC LIST - list USB services with state and endpoint cost.
 * \param args Unused.
 */
static void cmdUsbSvcList(const char* args) {
    (void)args;
    auto& mgr = core::UsbServiceManager::instance();

    Console::printf("=== USB Services (%u) ===\r\n", static_cast<unsigned>(mgr.count()));
    for (uint8_t i = 0; i < mgr.count(); i++) {
        const core::UsbServiceDesc* desc = mgr.at(i);
        if (!desc) continue;
        Console::printf("  %-8s %-12s IN:%u OUT:%u\r\n",
                        desc->id, usbSvcStateName(mgr.state(i)),
                        static_cast<unsigned>(desc->cost.in_eps),
                        static_cast<unsigned>(desc->cost.out_eps));
    }
    const usb_ep_usage_t usage = mgr.usage();
    Console::printf("Endpoints: IN %u/%u OUT %u/%u\r\n",
                    static_cast<unsigned>(usage.in_eps), USB_EP_BUDGET_MAX_IN,
                    static_cast<unsigned>(usage.out_eps), USB_EP_BUDGET_MAX_OUT);
}

/**
 * \brief Runs a USB service toggle and prints the classified result.
 * \param id Service id.
 * \param enabled Desired state.
 */
static void runUsbSvcToggle(const char* id, bool enabled) {
    using Result = core::UsbServiceManager::ToggleResult;

    bool needsReplugBefore = core::UsbManager::instance().needsReplug();

    switch (core::UsbServiceManager::instance().setEnabled(id, enabled)) {
        case Result::Ok:
            Console::printf("OK: Service '%s' %s\r\n", id, enabled ? "enabled" : "disabled");
            if (core::UsbManager::instance().newlyRequiresReplug(needsReplugBefore)) {
                Console::printf("NOTE: USB replug required\r\n");
            }
            return;
        case Result::BudgetFull:
            Console::printf("ERROR: USB endpoint budget exhausted - disable another service first\r\n");
            return;
        case Result::SlotBusy:
            Console::printf("ERROR: USB slot in use by a conflicting service\r\n");
            return;
        case Result::Busy:
            Console::printf("ERROR: Service busy (endpoint borrowed) - try again later\r\n");
            return;
        case Result::NotFound:
            Console::printf("ERROR: Service '%s' not found (see USBSVC LIST)\r\n", id);
            return;
        case Result::Failed:
        default:
            Console::printf("ERROR: Failed to %s service '%s'\r\n",
                            enabled ? "enable" : "disable", id);
            return;
    }
}

/**
 * \brief USBSVC ENABLE <id> - enable a USB service (persistent).
 * \param args Service id.
 */
static void cmdUsbSvcEnable(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: USBSVC ENABLE <id>\r\n");
        return;
    }
    runUsbSvcToggle(args, true);
}

/**
 * \brief USBSVC DISABLE <id> [CONFIRM] - disable a USB service (persistent).
 *
 * Disabling "cdc" kills this serial console immediately, so it demands an
 * explicit CONFIRM argument; recovery is Tools > USB Services on the badge.
 * \param args Service id, optionally followed by CONFIRM.
 */
static void cmdUsbSvcDisable(const char* args) {
    if (!args || !*args) {
        Console::printf("Usage: USBSVC DISABLE <id> [CONFIRM]\r\n");
        return;
    }

    char id[16];
    bool confirmed = false;
    const char* space = strchr(args, ' ');
    if (space) {
        size_t len = static_cast<size_t>(space - args);
        if (len >= sizeof(id)) len = sizeof(id) - 1;
        memcpy(id, args, len);
        id[len] = '\0';
        confirmed = (strcmp(space + 1, "CONFIRM") == 0);
    } else {
        strlcpy(id, args, sizeof(id));
    }

    if (strcmp(id, "cdc") == 0 && !confirmed) {
        Console::printf("WARNING: Disabling 'cdc' kills this serial console immediately.\r\n");
        Console::printf("Re-enable via badge menu: Tools > USB Services.\r\n");
        Console::printf("To proceed: USBSVC DISABLE cdc CONFIRM\r\n");
        return;
    }

    runUsbSvcToggle(id, false);
}

/**
 * \brief Sub-command tables and dispatchers for grouped commands.
 */

static const SubCommand kNvsSubs[] = {
    {"LIST",  "[namespace]",   "List entries (optional namespace filter)", cmdNvsList},
    {"READ",  "<ns> <key>",    "Read key value",                            cmdNvsRead},
    {"DEL",   "<ns> [key]",    "Delete key, or entire namespace if omitted",cmdNvsDel},
    {"CLEAR", "YES",           "Erase entire NVS (confirmation required)",  cmdNvsClear},
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdNvs(const char* args) { dispatchSubCommand("NVS", args, kNvsSubs); }

static const SubCommand kPinSubs[] = {
    {"STATUS",       "",                      "Show PIN retries / lockout state",         cmdPinStatus},
    {"RESET",        "",                      "Reset PIN retries (debug)",                cmdPinReset},
    {"CHANGE",       "<currentPin> <newPin>", "Change badge PIN (4-8 digits)",            cmdPinChange},
    {"DURESS",       "<pin>",                 "Arm self-destruct PIN (wipes on entry)",   cmdPinDuress},
    {"DURESS_CLEAR", "",                      "Disarm the self-destruct PIN",             cmdPinDuressClear},
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdPin(const char* args) { dispatchSubCommand("PIN", args, kPinSubs); }

static const SubCommand kTr01Subs[] = {
    {"STATUS",        "",         "Show TR01 connection status",                   cmdTr01Status},
    {"INFO",          "",         "Show TR01 chip info (ID, firmware)",            cmdTr01Info},
    {"SESSION",       "",         "Start/restart TR01 session",                    cmdTr01Session},
    {"SLOTS",         "",         "Show TR01 slot usage summary",                  cmdTr01Slots},
    {"RMEM_READ",     "<slot>",   "Read and dump R-Memory slot",                   cmdTr01RmemRead},
    {"ECC_DEL",       "<slot>",   "Delete ECC key slot",                           cmdTr01EccDel},
    {"RMEM_DEL",      "<slot>",   "Delete R-Memory slot",                          cmdTr01RmemDel},
    {"RESYNC",        "",         "Resync TR01 session and cache",                 cmdTr01Resync},
    {"CACHE_REBUILD", "",         "Rebuild TR01 cache from chip",                  cmdTr01CacheRebuild},
    {"CLEANUP",       "",         "Cleanup mismatched slots and rebuild cache",    cmdTr01Cleanup},
    {"WIPE",          "CONFIRM",  "Factory reset all TR01 data",                   cmdTr01Wipe},
#if FEATURE_PROVISIONING
    {"PAIR_WRITE",    "<slot> <hexpub>", "Write host pubkey to empty pairing slot (1-shot)", cmdTr01PairWrite},
    {"PAIR_INVALIDATE", "<slot> CONFIRM", "PERMANENTLY invalidate a pairing slot",       cmdTr01PairInvalidate},
#endif
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdTr01(const char* args) { dispatchSubCommand("TR01", args, kTr01Subs); }

static const SubCommand kWifiSubs[] = {
    {"SCAN",    "",                       "Scan for available networks",                       cmdWifiScan},
    {"STATUS",  "",                       "Show WiFi state and saved configuration",           cmdWifiStatus},
    {"ON",      "[sta|ap|sta_ap]",        "Enable WiFi radio (default STA, auto-reconnect)",   cmdWifiOn},
    {"OFF",     "",                       "Disable WiFi radio",                                cmdWifiOff},
    {"CONNECT", "<ssid> <password>",      "Connect to network and persist credentials",        cmdWifiConnect},
    {"TIMEOUT", "[ms]",                   "Get or set connect timeout (3000-60000 ms)",        cmdWifiTimeout},
    {"FORGET",  "",                       "Clear saved WiFi configuration",                    cmdWifiForget},
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdWifi(const char* args) { dispatchSubCommand("WIFI", args, kWifiSubs); }

static const SubCommand kModuleSubs[] = {
    {"LIST",    "",        "List modules with state and errors", cmdModuleList},
    {"ENABLE",  "<name>",  "Enable a module (persistent)",       cmdModuleEnable},
    {"DISABLE", "<name>",  "Disable a module (persistent)",      cmdModuleDisable},
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdModule(const char* args) { dispatchSubCommand("MODULE", args, kModuleSubs); }

static const SubCommand kUsbSvcSubs[] = {
    {"LIST",    "",               "List USB services with state and endpoint cost", cmdUsbSvcList},
    {"ENABLE",  "<id>",           "Enable a USB service (persistent)",              cmdUsbSvcEnable},
    {"DISABLE", "<id> [CONFIRM]", "Disable a USB service (CONFIRM needed for cdc)", cmdUsbSvcDisable},
    {nullptr, nullptr, nullptr, nullptr},
};
static void cmdUsbSvc(const char* args) { dispatchSubCommand("USBSVC", args, kUsbSvcSubs); }

/**
 * \brief Registers all built-in serial commands.
 */
void SerialCmd::registerBuiltinCommands() {
    auto& reg = getCommandRegistry();

    reg.registerCommand({"HELP", "Show available commands", cmdHelp, "system", false});
    reg.registerCommand({"PING", "Check if device is responsive", cmdPing, "system", false});
    reg.registerCommand({"VERSION", "Show firmware version and API level", cmdVersion, "system", false});
    reg.registerCommand({"STATUS", "Show system status", cmdStatus, "system", false});
    reg.registerCommand({"MEM", "Show memory usage", cmdMem, "system", false});
    reg.registerCommand({"MEMINFO", "Show detailed memory + task info", cmdMemInfo, "system", false});
    reg.registerCommand({"CPU", "Measure aggregate CPU load (~250 ms)", cmdCpu, "system", false});
    reg.registerCommand({"ERROR_LOG", "Show error log (CLEAR to reset)", cmdErrorLog, "system", false});
    reg.registerCommand({"REBOOT", "Restart the device", cmdReboot, "system", true});
    reg.registerCommand({"BOOTLOADER", "Reboot into USB download mode", cmdBootloader, "system", true});
    reg.registerCommand({"SHIPMODE", "Enter ship mode (disconnect battery)", cmdShipMode, "system", true});
    reg.registerCommand({"PASTE", "Paste text into the active T9 input", cmdPaste, "system", true});

    reg.registerCommand({"NVS", "NVS storage: LIST/READ/DEL/CLEAR", cmdNvs, "nvs", true, kNvsSubs});

    reg.registerCommand({"GET_TIME", "Show current time", cmdGetTime, "time", false});
    reg.registerCommand({"GET_DATE", "Show current date", cmdGetDate, "time", false});
    reg.registerCommand({"SET_TIME", "Set time (HH:MM:SS)", cmdSetTime, "time", false});
    reg.registerCommand({"SET_DATE", "Set date (DD.MM.YYYY or Unix timestamp)", cmdSetDate, "time", false});

    reg.registerCommand({"SET_NAME", "Set display name", cmdSetName, "display", false});
    reg.registerCommand({"SET_INFO", "Set info line 1", cmdSetInfo, "display", false});
    reg.registerCommand({"SET_INFO2", "Set info line 2", cmdSetInfo2, "display", false});

    reg.registerCommand({"PIN", "PIN management: STATUS/RESET/CHANGE/DURESS", cmdPin, "pin", true, kPinSubs});

    reg.registerCommand({"TR01", "TROPIC01 secure element: STATUS/INFO/SESSION/SLOTS/RMEM_*/ECC_DEL/...",
                         cmdTr01, "tr01", true, kTr01Subs});

#if FEATURE_SECURE_SERIAL
    reg.registerCommand({"AUTH", "Authenticate with PIN", cmdAuth, "auth", false});
    reg.registerCommand({"LOGOUT", "End authenticated session", cmdLogout, "auth", false});
#endif

    reg.registerCommand({"WIFI", "WiFi control: SCAN/STATUS/ON/OFF/CONNECT/TIMEOUT/FORGET",
                         cmdWifi, "wifi", true, kWifiSubs});

    reg.registerCommand({"MODULE", "Module control: LIST/ENABLE/DISABLE",
                         cmdModule, "module", true, kModuleSubs});

    reg.registerCommand({"USBSVC", "USB services: LIST/ENABLE/DISABLE",
                         cmdUsbSvc, "usb", true, kUsbSvcSubs});
}

} // namespace cdc::serial
