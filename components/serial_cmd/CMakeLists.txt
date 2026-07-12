# CDC Badge OS - Serial Command Interface
# Modular command registration and processing

idf_component_register(
    SRCS
        "src/CommandRegistry.cpp"
        "src/Console.cpp"
        "src/SerialCmd.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        bootloader_support
        driver
        freertos
        esp_timer
        usb_badge
        cdc_log
        cdc_os_ui
        cdc_views
)
