# CDC Badge OS - Core Component
# ServiceRegistry, EventBus

idf_component_register(
    SRCS
        "src/AttestationKeyService.cpp"
        "src/ServiceRegistry.cpp"
        "src/Cp437.cpp"
        "src/EventBus.cpp"
        "src/PinManager.cpp"
        "src/ModuleRegistry.cpp"
        "src/SystemLock.cpp"
        "src/TropicStorage.cpp"
        "src/TropicSlotMap.cpp"
        "src/KeyFingerprint.cpp"
        "src/UsbManager.cpp"
        "src/IKeyboardProvider.cpp"
        "src/pin_storage_c.cpp"
        "src/FactoryReset.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        esp_timer
        esp_hw_support
        nvs_flash
        cdc_hal
        cdc_log
        mbedtls
)
