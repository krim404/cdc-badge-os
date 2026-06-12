#include "cdc_core/ServiceRegistry.h"
#include "cdc_log.h"
#include <cstring>

static const char* TAG = "ServiceRegistry";

namespace cdc::core {

/**
 * \brief Converts a service type enum to a log-friendly string.
 * \param type Service type to convert.
 * \return Constant string representation of the service type.
 */
static const char* serviceTypeName(ServiceType type) {
    switch (type) {
        case ServiceType::KEYBOARD:            return "keyboard";
        case ServiceType::CHALLENGE_RESPONDER: return "challenge_responder";
        case ServiceType::CLIPBOARD:           return "clipboard";
        case ServiceType::NOTIFICATION:        return "notification";
        default:                               return "unknown";
    }
}

/**
 * \brief Returns singleton service registry instance.
 * \return Reference to global `ServiceRegistry`.
 */
ServiceRegistry& ServiceRegistry::instance() {
    static ServiceRegistry instance;
    return instance;
}

/**
 * \brief Registers a named service instance.
 * \param name Service name key.
 * \param service Service implementation pointer.
 * \return `true` if registration succeeded.
 */
bool ServiceRegistry::registerService(const char* name, IService* service) {
    if (!name || !service) {
        LOG_E(TAG, "Invalid parameters");
        return false;
    }

    if (count_ >= MAX_SERVICES) {
        LOG_E(TAG, "Registry full, cannot register '%s'", name);
        return false;
    }

    // Check for duplicate name
    for (size_t i = 0; i < count_; i++) {
        if (strcmp(services_[i].name, name) == 0) {
            LOG_E(TAG, "Service '%s' already registered", name);
            return false;
        }
    }

    services_[count_].name = name;
    services_[count_].service = service;
    count_++;

    LOG_I(TAG, "Registered service '%s'", name);
    return true;
}

/**
 * \brief Looks up a service by name.
 * \param name Service name key.
 * \return Matching service pointer or `nullptr`.
 */
IService* ServiceRegistry::getService(const char* name) {
    if (!name) return nullptr;

    for (size_t i = 0; i < count_; i++) {
        if (strcmp(services_[i].name, name) == 0) {
            return services_[i].service;
        }
    }

    return nullptr;
}

/**
 * \brief Typed service retrieval and registration helpers.
 */

/**
 * \brief Registers typed service pointer.
 * \param type Typed service slot.
 * \param service Service implementation pointer.
 * \return `true` if registration succeeded.
 */
bool ServiceRegistry::registerTypedService(ServiceType type, void* service) {
    size_t idx = static_cast<size_t>(type);
    if (idx >= MAX_TYPED_SERVICES) {
        LOG_E(TAG, "Invalid service type %d", static_cast<int>(type));
        return false;
    }

    if (typedServices_[idx] != nullptr) {
        LOG_W(TAG, "Replacing existing %s service", serviceTypeName(type));
    }

    typedServices_[idx] = service;
    LOG_I(TAG, "Provided %s service", serviceTypeName(type));
    return true;
}

/**
 * \brief Returns typed service pointer.
 * \param type Typed service slot.
 * \return Stored pointer or `nullptr`.
 */
void* ServiceRegistry::getTypedService(ServiceType type) const {
    size_t idx = static_cast<size_t>(type);
    if (idx >= MAX_TYPED_SERVICES) {
        return nullptr;
    }
    return typedServices_[idx];
}

/**
 * \brief Checks whether typed service exists.
 * \param type Typed service slot.
 * \return `true` if available.
 */
bool ServiceRegistry::isAvailable(ServiceType type) const {
    return getTypedService(type) != nullptr;
}

/**
 * \brief Initializes all registered services in registration order.
 * \return `true` if all services initialized successfully.
 */
bool ServiceRegistry::initAll() {
    LOG_I(TAG, "Initializing %u services...", count_);

    for (size_t i = 0; i < count_; i++) {
        LOG_I(TAG, "  [%u/%u] %s", i + 1, count_, services_[i].name);

        if (!services_[i].service->init()) {
            LOG_E(TAG, "Failed to initialize '%s'", services_[i].name);
            return false;
        }
    }

    LOG_I(TAG, "All services initialized");
    return true;
}

/**
 * \brief Starts all initialized services.
 * \return `true` if all services started successfully.
 */
bool ServiceRegistry::startAll() {
    LOG_I(TAG, "Starting %u services...", count_);

    for (size_t i = 0; i < count_; i++) {
        if (services_[i].service->getState() == ServiceState::INITIALIZED ||
            services_[i].service->getState() == ServiceState::STOPPED) {

            if (!services_[i].service->start()) {
                LOG_E(TAG, "Failed to start '%s'", services_[i].name);
                return false;
            }
        }
    }

    LOG_I(TAG, "All services started");
    return true;
}

/**
 * \brief Stops started services in reverse registration order.
 * \return void
 */
void ServiceRegistry::stopAll() {
    LOG_I(TAG, "Stopping %u services...", count_);

    // Stop in reverse order
    for (size_t i = count_; i > 0; i--) {
        if (services_[i - 1].service->getState() == ServiceState::STARTED) {
            services_[i - 1].service->stop();
        }
    }

    LOG_I(TAG, "All services stopped");
}

} // namespace cdc::core
