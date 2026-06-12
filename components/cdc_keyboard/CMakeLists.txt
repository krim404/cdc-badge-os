# CDC Keyboard Core - Transport-agnostic HID keyboard logic
# Report descriptor, US-QWERTY layout tables, Unicode input methods and the
# typeString engine shared by the BLE and USB HID keyboard transport modules.

idf_component_register(
    SRCS
        "src/KeyboardEngine.cpp"
        "src/KeyboardLayout.cpp"
        "src/HidReportMap.cpp"
        "src/KeyboardProviderBase.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        cdc_core
        nvs_flash
)
