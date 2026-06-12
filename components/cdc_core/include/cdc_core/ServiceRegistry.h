#pragma once

#include "IService.h"
#include <cstddef>

namespace cdc::core {

/**
 * Well-known service types for typed service discovery.
 * Use these with provide<T>() and request<T>() for type-safe inter-module communication.
 */
enum class ServiceType {
    KEYBOARD,             // IKeyboardProvider - keyboard input (BLE HID, USB HID, etc.)
    CHALLENGE_RESPONDER,  // IChallengeResponder - raw HMAC challenge-response (CR transports)
    CLIPBOARD,            // Future: clipboard access
    NOTIFICATION,         // Future: push notifications
};

/**
 * Service Locator / Dependency Injection container
 *
 * Manages service registration/discovery and provides typed access.
 * Uses static allocation - no heap.
 *
 * Two usage patterns:
 * 1. Named services: registerService("display", &display) / get<IDisplay>("display")
 * 2. Typed services: provide<IKeyboardProvider>(ServiceType::KEYBOARD, &kb) / request<IKeyboardProvider>(ServiceType::KEYBOARD)
 */
class ServiceRegistry {
public:
    static constexpr size_t MAX_SERVICES = 24;

    /**
     * Get singleton instance
     */
    static ServiceRegistry& instance();

    // =========================================================================
    // Named Service Pattern (for core HAL services)
    // =========================================================================

    /**
     * Register a service by name
     * \param name Unique service name (e.g., "display", "keypad")
     * \param service Pointer to service instance (must outlive registry)
     * \return true on success, false if full or duplicate name
     */
    bool registerService(const char* name, IService* service);

    /**
     * Get service by name (untyped)
     * \return nullptr if not found
     */
    IService* getService(const char* name);

    /**
     * Get service by name (typed)
     * Usage: auto* display = registry.get<IDisplay>("display");
     */
    template<typename T>
    T* get(const char* name) {
        return static_cast<T*>(getService(name));
    }

    // =========================================================================
    // Typed Service Pattern (for optional inter-module services)
    // =========================================================================

    /**
     * Provide a typed service ("I offer service X")
     * \param type The service type to register
     * \param service Pointer to service implementation
     * \return true on success
     */
    template<typename T>
    bool provide(ServiceType type, T* service) {
        return registerTypedService(type, service);
    }

    /**
     * Request a typed service ("I need service X")
     * \param type The service type to request
     * \return Pointer to service or nullptr if not available
     */
    template<typename T>
    T* request(ServiceType type) {
        return static_cast<T*>(getTypedService(type));
    }

    /**
     * Check if a typed service is available
     */
    bool isAvailable(ServiceType type) const;

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /**
     * Initialize all registered services
     * \return true if all succeeded
     */
    bool initAll();

    /**
     * Start all registered services
     * \return true if all succeeded
     */
    bool startAll();

    /**
     * Stop all registered services (reverse order)
     */
    void stopAll();

    /**
     * Get number of registered services
     */
    size_t count() const { return count_; }

private:
    ServiceRegistry() = default;
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    // Internal typed service registration
    bool registerTypedService(ServiceType type, void* service);
    void* getTypedService(ServiceType type) const;

    struct Entry {
        const char* name;
        IService* service;
    };

    Entry services_[MAX_SERVICES] = {};
    size_t count_ = 0;

    // Typed services storage (indexed by ServiceType)
    static constexpr size_t MAX_TYPED_SERVICES = 8;
    void* typedServices_[MAX_TYPED_SERVICES] = {};
};

// Convenience macros for service access
#define CDC_SERVICE(type, name) \
    cdc::core::ServiceRegistry::instance().get<type>(name)

#define CDC_PROVIDE_SERVICE(type, ptr) \
    cdc::core::ServiceRegistry::instance().provide<decltype(*(ptr))>(type, ptr)

#define CDC_REQUEST_SERVICE(type, T) \
    cdc::core::ServiceRegistry::instance().request<T>(type)

} // namespace cdc::core
