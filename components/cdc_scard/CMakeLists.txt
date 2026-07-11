# Smartcard Component - CCID transport, APDU parser, applet dispatcher

idf_component_register(
    SRCS
        "src/scard.cpp"
        "src/scard_usb.cpp"
        "src/ccid.cpp"
        "src/ccid_driver.cpp"
        "src/apdu.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_log
        freertos
        espressif__tinyusb
    PRIV_REQUIRES
        usb
)
