# Badge-to-badge message transfer framework (BLE, MIME-typed)

idf_component_register(
    SRCS
        "src/MessageTransfer.cpp"
        "src/MessageHandlerRegistry.cpp"
        "src/BeaconManager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_hal
        cdc_log
        nvs_flash
        freertos
)
