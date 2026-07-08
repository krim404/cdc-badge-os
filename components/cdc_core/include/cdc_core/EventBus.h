#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::core {

/**
 * Event types for system-wide communication
 */
// The ordinal of each enumerator IS the wire value: EventBus::eventMask and
// the plugin host API expose events as (1u << ordinal). Plugin-facing events
// come FIRST so their public EVENT_* bits (host_api.h) stay contiguous with no
// gaps; internal-only events (never surfaced to plugins) follow. Keep this
// split when adding events - a static_assert in host_api_event.cpp locks the
// public bits to these ordinals.
enum class EventType : uint8_t {
    // --- Plugin-facing events (mirrored by EVENT_* in host_api.h) ---
    // Input events
    KEY_PRESSED,
    KEY_RELEASED,
    KEY_LONG_PRESS,

    // Power events
    POWER_USB_CONNECTED,
    POWER_USB_DISCONNECTED,
    POWER_CHARGING,
    POWER_BATTERY_LOW,
    POWER_BATTERY_CRITICAL,

    // System events
    SYSTEM_UNLOCK,
    SYSTEM_LOCK,
    SYSTEM_SLEEP,
    SYSTEM_WAKE,

    // Bluetooth events
    BLE_CONNECTED,
    BLE_DISCONNECTED,

    // Timer
    TIMER_TICK,

    // Custom module events (use data.value for sub-type)
    MODULE_EVENT,

    // Display refresh in progress (data.value = 1 begin, 0 end); published
    // by the e-paper driver for FAST/FULL refreshes so plugins can pause.
    DISPLAY_REFRESH,

    // --- Internal-only events (no public EVENT_* bit) ---
    SYSTEM_SLEEP_INCOMING,
    BLE_PAIRING_REQUEST,
    BLE_CONSENT_REQUEST,
    BLE_EXCHANGE_COMPLETE,
    // Module error (data.ptr = module name, data.value = index)
    MODULE_ERROR,

    EVENT_COUNT
};

/**
 * Event data structure
 */
struct Event {
    EventType type;
    uint32_t timestamp;  // millis since boot
    union {
        char key;        // For KEY_* events
        uint8_t value;   // Generic value
        void* ptr;       // Pointer to extended data
    } data;
};

/**
 * Event handler function type
 */
using EventHandler = void(*)(const Event&);

/**
 * Simple publish/subscribe event bus
 *
 * - Supports ISR-safe publishing via FreeRTOS queue
 * - Static allocation, no heap
 * - Handlers are called from main loop (process())
 */
class EventBus {
public:
    static constexpr size_t MAX_HANDLERS = 16;
    static constexpr size_t DEFAULT_QUEUE_SIZE = 32;

    /**
     * Get singleton instance
     */
    static EventBus& instance();

    /**
     * Initialize the event bus
     * \param queueSize Number of events that can be queued
     * \return true on success
     */
    bool init(size_t queueSize = DEFAULT_QUEUE_SIZE);

    /**
     * Subscribe to events
     * \param handler Function to call when event occurs
     * \param mask Bitmask of EventTypes to receive (0 = all)
     * \return Handler ID (0 on failure)
     */
    uint8_t subscribe(EventHandler handler, uint32_t mask = 0);

    /**
     * Unsubscribe handler
     * \param id Handler ID from subscribe()
     */
    void unsubscribe(uint8_t id);

    /**
     * Publish an event
     * \param event Event to publish
     * \param fromISR true if called from interrupt context
     * \return true if event was queued
     */
    bool publish(const Event& event, bool fromISR = false);

    /**
     * Publish event (convenience)
     */
    bool publish(EventType type, uint8_t value = 0);

    /**
     * Process queued events (call from main loop)
     */
    void process();

    /**
     * Create event mask for subscribe()
     */
    static constexpr uint32_t eventMask(EventType type) {
        return 1u << static_cast<uint8_t>(type);
    }

private:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct Subscription {
        EventHandler handler;
        uint32_t mask;
        bool active;
    };

    Subscription handlers_[MAX_HANDLERS] = {};
    void* queue_ = nullptr;  // FreeRTOS QueueHandle_t
    bool initialized_ = false;
};

} // namespace cdc::core
