/**
 * \file
 * \brief Logging and console I/O implementation with optional hook transports.
 */
#include "cdc_log.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_TINYUSB_CDC_ENABLED
#include "tusb.h"
#endif

#ifndef DEBUG_MODE
#define DEBUG_MODE 1
#endif

#if DEBUG_MODE
static log_level_t s_log_level = CDC_LOG_LEVEL_DEBUG;
#else
static log_level_t s_log_level = CDC_LOG_LEVEL_WARN;
#endif
static bool s_initialized = false;

/** \brief Optional console hooks for additional I/O transports (for example BLE). */
static console_output_hook_t s_output_hook = nullptr;
static console_input_available_hook_t s_input_avail_hook = nullptr;
static log_authgate_hook_t s_authgate_hook = nullptr;
static console_input_getchar_hook_t s_input_getchar_hook = nullptr;

static const char* level_str[] = {
    "",      // NONE
    "E",     // ERROR
    "W",     // WARN
    "I",     // INFO
    "D",     // DEBUG
    "V"      // VERBOSE
};

/** \brief PSRAM-backed error/warn ring log storage (no heap allocation). */
EXT_RAM_BSS_ATTR static error_log_entry_t s_error_log[ERROR_LOG_MAX_ENTRIES];
static size_t s_error_log_head = 0;  // Next write position
static size_t s_error_log_count = 0; // Number of entries

/**
 * \brief Adds warning/error entry to PSRAM-backed ring log.
 * \param level Log level.
 * \param message Formatted message text.
 */
static void error_log_add(log_level_t level, const char* message) {
    if (level != CDC_LOG_LEVEL_ERROR && level != CDC_LOG_LEVEL_WARN) return;
    if (!message) return;

    // Write to current position (overwrites oldest if full)
    error_log_entry_t* entry = &s_error_log[s_error_log_head];
    entry->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    entry->level = level;
    strncpy(entry->message, message, ERROR_LOG_LINE_LEN - 1);
    entry->message[ERROR_LOG_LINE_LEN - 1] = '\0';

    // Advance head (ring buffer)
    s_error_log_head = (s_error_log_head + 1) % ERROR_LOG_MAX_ENTRIES;
    if (s_error_log_count < ERROR_LOG_MAX_ENTRIES) {
        s_error_log_count++;
    }
}

/**
 * \brief Copies stored error-log entries in chronological order.
 * \param entries Output entry buffer.
 * \param max_entries Maximum writable entries.
 * \return Number of copied entries.
 */
size_t error_log_get_entries(error_log_entry_t* entries, size_t max_entries) {
    if (!entries || max_entries == 0) return 0;

    size_t count = 0;
    // Calculate start position (oldest entry)
    size_t start = (s_error_log_head + ERROR_LOG_MAX_ENTRIES - s_error_log_count) % ERROR_LOG_MAX_ENTRIES;

    for (size_t i = 0; i < s_error_log_count && count < max_entries; i++) {
        size_t idx = (start + i) % ERROR_LOG_MAX_ENTRIES;
        entries[count++] = s_error_log[idx];
    }
    return count;
}

/**
 * \brief Returns number of buffered error-log entries.
 * \return Entry count.
 */
size_t error_log_get_count(void) {
    return s_error_log_count;
}

/**
 * \brief Clears error-log ring buffer state.
 */
void error_log_clear(void) {
    s_error_log_head = 0;
    s_error_log_count = 0;
}

/**
 * \brief Dumps buffered error-log entries to console.
 */
void error_log_dump(void) {
    if (s_error_log_count == 0) {
        console_printf("Error log: (empty)\r\n");
        return;
    }

    console_printf("Error log (%zu entries):\r\n", s_error_log_count);

    size_t start = (s_error_log_head + ERROR_LOG_MAX_ENTRIES - s_error_log_count) % ERROR_LOG_MAX_ENTRIES;
    for (size_t i = 0; i < s_error_log_count; i++) {
        size_t idx = (start + i) % ERROR_LOG_MAX_ENTRIES;
        error_log_entry_t* e = &s_error_log[idx];

        uint32_t secs = e->timestamp_ms / 1000;
        uint32_t mins = secs / 60;
        uint32_t hours = mins / 60;
        console_printf("[%02lu:%02lu:%02lu][%s] %s\r\n",
                       hours % 24, mins % 60, secs % 60,
                       e->level == CDC_LOG_LEVEL_ERROR ? "E" : "W",
                       e->message);
    }
}

/** \brief Logging API implementation. */

/**
 * \brief Initializes logging subsystem and console backend.
 */
void log_init(void) {
#if DEBUG_MODE
    s_log_level = CDC_LOG_LEVEL_DEBUG;
#else
    s_log_level = CDC_LOG_LEVEL_WARN;
#endif
    console_init();
}

/**
 * \brief Sets runtime log verbosity threshold.
 * \param level New log level.
 */
void log_set_level(log_level_t level) {
    s_log_level = level;
}

/**
 * \brief Returns current log verbosity threshold.
 * \return Active log level.
 */
log_level_t log_get_level(void) {
    return s_log_level;
}

/**
 * \brief Writes formatted tagged log line with optional suppression.
 * \param level Log level.
 * \param tag Log tag.
 * \param fmt Printf-style format string.
 */
void log_write(log_level_t level, const char* tag, const char* fmt, ...) {
    // Always capture ERROR/WARN to error log
    bool capture = (level == CDC_LOG_LEVEL_ERROR || level == CDC_LOG_LEVEL_WARN);

    // Format message
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Format with prefix
    char line[300];
    snprintf(line, sizeof(line), "[%s][%s] %s",
             level_str[level], tag ? tag : "???", buf);

    // Capture to error log (before suppression check)
    if (capture) {
        error_log_add(level, line);
    }

    // Suppress INFO/DEBUG/VERBOSE while the auth-gate hook (if installed)
    // reports an unauthenticated state. ERROR/WARN always emit.
    if (level > CDC_LOG_LEVEL_WARN && s_authgate_hook && !s_authgate_hook()) {
        return;
    }

    // Output if not suppressed
    if (level <= s_log_level) {
        console_printf("%s\n", line);
    }
}

/**
 * \brief Writes untagged raw formatted text to console.
 * \param fmt Printf-style format string.
 */
void log_raw(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_print(buf);
}

/**
 * \brief Logs binary buffer as grouped hexadecimal bytes.
 * \param tag Log tag.
 * \param label Data label.
 * \param data Input byte buffer.
 * \param len Buffer length.
 */
void log_hex(const char* tag, const char* label, const uint8_t* data, size_t len) {
#if !DEBUG_MODE
    // Release build: hex dumps can leak key material, swallow silently.
    (void)tag; (void)label; (void)data; (void)len;
#else
    console_printf("[D][%s] %s (%zu bytes): ", tag ? tag : "HEX", label ? label : "data", len);
    for (size_t i = 0; i < len; i++) {
        console_printf("%02X", data[i]);
        if ((i + 1) % 32 == 0 && (i + 1) < len) {
            console_print("\n      ");
        } else if ((i + 1) % 4 == 0 && (i + 1) < len) {
            console_putchar(' ');
        }
    }
    console_print("\n");
#endif
}

/**
 * \brief Initializes console I/O transport state.
 */
void console_init(void) {
    if (s_initialized) return;

    // Set stdin to non-blocking for UART fallback
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    s_initialized = true;
}

/**
 * \brief Returns whether any console input source has pending data.
 * \return `true` if input is available.
 */
bool console_available(void) {
    if (!s_initialized) return false;

#if CONFIG_TINYUSB_CDC_ENABLED
    if (tud_cdc_connected() && tud_cdc_available() > 0) {
        return true;
    }
#endif

    // Check input hook (e.g., BLE)
    if (s_input_avail_hook && s_input_avail_hook()) {
        return true;
    }

    return false;
}

/**
 * \brief Reads one character from available console input source.
 * \return Character value or `-1` when no data is available.
 */
int console_getchar(void) {
    if (!s_initialized) return -1;

#if CONFIG_TINYUSB_CDC_ENABLED
    // Priority 1: USB CDC
    if (tud_cdc_connected() && tud_cdc_available()) {
        return tud_cdc_read_char();
    }
#endif

    // Priority 2: Input hook (e.g., BLE)
    if (s_input_getchar_hook) {
        int c = s_input_getchar_hook();
        if (c >= 0) {
            return c;
        }
    }

    // Fallback: UART via stdin
    int c = getchar();
    if (c != EOF) {
        return c;
    }
    return -1;
}

/**
 * \brief Writes string to active console outputs.
 * \param str Null-terminated string.
 */
void console_print(const char* str) {
    if (!str) return;
    size_t len = strlen(str);

    // Always output to UART (visible via USB-Serial-JTAG or external adapter)
    printf("%s", str);
    fflush(stdout);

#if CONFIG_TINYUSB_CDC_ENABLED
    // Also send to USB CDC if connected. If the host stops reading the TX
    // FIFO fills up; without a bound the original `while (avail == 0)
    // continue;` loop deadlocks the caller — and because logging happens on
    // every task this drags the whole UART driver lock along, freezing the
    // device end-to-end. We give the FIFO a short retry window and then drop
    // the rest of this log line: dropping a log entry is non-fatal, but
    // hanging on it is.
    if (s_initialized && tud_cdc_connected()) {
        size_t written = 0;
        const TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(20);
        while (written < len) {
            size_t avail = tud_cdc_write_available();
            if (avail == 0) {
                tud_cdc_write_flush();
                if (xTaskGetTickCount() >= deadline) break;
                vTaskDelay(1);
                continue;
            }
            size_t to_write = len - written;
            if (to_write > avail) to_write = avail;
            written += tud_cdc_write(str + written, to_write);
        }
        tud_cdc_write_flush();
    }
#endif

    // Also send to output hook (e.g., BLE)
    if (s_output_hook) {
        s_output_hook(str, len);
    }
}

/**
 * \brief Formatted write helper for console output.
 * \param fmt Printf-style format string.
 */
void console_printf(const char* fmt, ...) {
    if (!fmt) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0 && static_cast<size_t>(len) >= sizeof(buf)) {
        char* heap = static_cast<char*>(malloc(static_cast<size_t>(len) + 1));
        if (heap) {
            vsnprintf(heap, static_cast<size_t>(len) + 1, fmt, args_copy);
            console_print(heap);
            free(heap);
            va_end(args_copy);
            return;
        }
    }
    va_end(args_copy);
    console_print(buf);
}

/**
 * \brief Writes single character to active console outputs.
 * \param c Character to output.
 */
void console_putchar(char c) {
    putchar(c);
    fflush(stdout);  // Immediate echo for serial terminal

#if CONFIG_TINYUSB_CDC_ENABLED
    if (s_initialized && tud_cdc_connected()) {
        tud_cdc_write_char(c);
        tud_cdc_write_flush();  // Immediate echo for USB CDC
    }
#endif

    // Also send to output hook (e.g., BLE)
    if (s_output_hook) {
        s_output_hook(&c, 1);
    }
}

/**
 * \brief Flushes buffered console output transports.
 */
void console_flush(void) {
#if CONFIG_TINYUSB_CDC_ENABLED
    if (s_initialized && tud_cdc_connected()) {
        tud_cdc_write_flush();
    }
#endif
    fflush(stdout);
}

/** \brief Console hook registration API. */
/**
 * \brief Registers optional additional output transport hook.
 * \param hook Output callback.
 */
void console_register_output_hook(console_output_hook_t hook) {
    s_output_hook = hook;
}

/**
 * \brief Registers optional additional input transport hooks.
 * \param avail_hook Input-available callback.
 * \param getchar_hook Character-read callback.
 */
void console_register_input_hook(console_input_available_hook_t avail_hook,
                                  console_input_getchar_hook_t getchar_hook) {
    s_input_avail_hook = avail_hook;
    s_input_getchar_hook = getchar_hook;
}

/**
 * \brief Installs the auth-gate hook used to suppress INFO/DEBUG/VERBOSE
 *        output while a session is unauthenticated.
 * \param hook Callback returning true when the gate is open, false to drop
 *             the line. NULL disables gating.
 */
void log_register_authgate_hook(log_authgate_hook_t hook) {
    s_authgate_hook = hook;
}
