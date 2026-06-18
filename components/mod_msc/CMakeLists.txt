# USB Mass Storage Module
# Exposes the vfat volume to a host as a removable USB drive (default-disabled).

idf_component_register(
    SRCS
        "src/UsbMscModule.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
        usb_badge
        plugin_manager
        nvs_flash
        freertos
)
