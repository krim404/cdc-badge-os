# BLE HID Keyboard Module
# Provides IKeyboardProvider service for auto-type functionality

idf_component_register(
    SRCS
        "src/BleHidModule.cpp"
        "src/BleHidKeyboard.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_keyboard
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
        nvs_flash
        freertos
)
