# USB HID Keyboard Module
# Provides IKeyboardProvider service over USB for auto-type functionality.

idf_component_register(
    SRCS
        "src/UsbHidModule.cpp"
        "src/UsbHidKeyboard.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_keyboard
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
        usb_badge
        nvs_flash
        freertos
)
