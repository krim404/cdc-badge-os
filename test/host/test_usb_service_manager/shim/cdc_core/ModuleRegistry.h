// Host-test shim for cdc_core/ModuleRegistry.h: fixed fake module table with
// call counters and failure knobs so the UsbServiceManager module-delegation
// path can be tested without the firmware module system.
#pragma once
#include <cstdint>
#include <cstring>

namespace cdc::core {

enum class ServiceState : uint8_t { STOPPED, INITIALIZED, STARTED, ERROR };

enum class ModuleStartFailure { SlotError, UsbBudgetFull, Generic };

class IModule {
public:
    const char* getName() const { return name; }
    ServiceState getState() const { return state; }
    void stop() {
        stopCalls++;
        state = ServiceState::STOPPED;
    }

    // Test state
    const char* name = "";
    ServiceState state = ServiceState::STOPPED;
    int stopCalls = 0;
};

class ModuleRegistry {
public:
    static constexpr uint8_t MAX_FAKE_MODULES = 4;

    static ModuleRegistry& instance() {
        static ModuleRegistry reg;
        return reg;
    }

    uint8_t getModuleCount() const { return count; }

    IModule* getModuleAt(uint8_t idx) { return idx < count ? &modules[idx] : nullptr; }

    bool isModuleEnabled(uint8_t idx) const { return idx < count && enabled[idx]; }

    void setModuleEnabled(uint8_t idx, bool on) {
        if (idx < count) enabled[idx] = on;
        setEnabledCalls++;
    }

    bool startModule(uint8_t idx) {
        startCalls++;
        if (startResult && idx < count) modules[idx].state = ServiceState::STARTED;
        return startResult;
    }

    ModuleStartFailure classifyStartFailure(uint8_t) const { return failure; }

    uint8_t addModule(const char* name) {
        modules[count].name = name;
        modules[count].state = ServiceState::INITIALIZED;
        modules[count].stopCalls = 0;
        enabled[count] = false;
        return count++;
    }

    void reset() {
        count = 0;
        startCalls = 0;
        setEnabledCalls = 0;
        startResult = true;
        failure = ModuleStartFailure::Generic;
        for (auto& m : modules) m = {};
        memset(enabled, 0, sizeof(enabled));
    }

    // Test knobs / counters
    IModule modules[MAX_FAKE_MODULES] = {};
    bool enabled[MAX_FAKE_MODULES] = {};
    uint8_t count = 0;
    bool startResult = true;
    ModuleStartFailure failure = ModuleStartFailure::Generic;
    int startCalls = 0;
    int setEnabledCalls = 0;
};

} // namespace cdc::core
