# USB OTP HID Module
# Emulates a YubiKey/OnlyKey OTP HID token for KeePassXC challenge-response.

idf_component_register(
    SRCS
        "src/OtpHidModule.cpp"
        "src/OtpHidInterface.cpp"
        "src/OtpHidCr.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
        usb_badge
        freertos
)
